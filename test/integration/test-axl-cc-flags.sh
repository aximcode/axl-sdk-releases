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

# 4a. --help names the ESCAPE HATCH, not only the default.
#
# The C++ note said "Still -fno-exceptions / -fno-rtti" and stopped there, which
# reads as a ceiling rather than a default -- a consumer evaluating whether AXL
# supports exceptions concluded from exactly that line, plus the same flags on
# the compile command, that it does not. It has since 1af166de: `-fexceptions`
# is a real gcc flag the caller passes and consumer flags append last, so it
# wins, and axl-cc derives the exceptions-capable link from it.
#
# Pinned by test because the failure is silent: help text that omits a
# capability costs a consumer a spike, and nothing else in the suite reads it.
# Both halves are asserted -- naming -fexceptions without keeping "off by
# default" would mislead in the other direction.
#
# The second phrase is spelled in full rather than as a bare "off by default",
# which the RTTI paragraph already contains: that shorter grep PASSED against
# help text saying nothing whatever about exceptions, i.e. it asserted nothing.
"$AXL_CC" --help >"$WORK/help.txt" 2>&1
grep -q -- '-fexceptions for real try/catch' "$WORK/help.txt"
check "$?" "--help names -fexceptions as the way to turn exceptions on"
grep -q -- 'Exceptions are off by default' "$WORK/help.txt"
check "$?" "--help still says exceptions are off by default"

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

# 5. Dependency generation is gcc's, forwarded — there is no axl-cc flag.
#
# There WAS a `--depfile` that post-processed the .d to absolutize every path,
# added because a RELATIVE source makes gcc emit compile-cwd-relative
# prerequisites that CMake's DEPFILE resolved against the wrong directory. It
# is gone: the generated CMake package passes ABSOLUTE sources, so gcc's own
# output is already absolute and there is nothing to rewrite.
#
# What these pin is the behaviour a consumer now depends on directly, and the
# -MMD/-MD distinction is the load-bearing part: --depfile used -MMD
# INTERNALLY, so it silently tracked no SDK header at all. Editing an SDK
# header did not rebuild a CMake consumer's object -- caught by
# test-cmake-package.sh, not here, because only an end-to-end rebuild shows it.
#
# Host-independent by construction: these assert COUNTS (zero vs non-zero) and
# the shape of a path, never a particular one, so they do not depend on the
# host gcc's include layout.
DW="$WORK/dep"
mkdir -p "$DW"
echo 'int helper(void);' > "$DW/t.h"
printf '#include "t.h"\n#include <axl.h>\nint helper(void){ return 0; }\n' > "$DW/t.c"

dep_tokens() { tr -d '\\' < "$1" | tr ' \t' '\n' | grep -vxE '.*:|' || true; }

# A RELATIVE source, compiled from its own directory: the shape a hand-written
# Makefile uses, where cwd-relative prerequisites are exactly right.
( cd "$DW" && "$AXL_CC" -c -MD -MP -MF t.d t.c -o t.o ) >"$DW/dep.log" 2>&1
df_rc=$?
check "$df_rc" "forwarded -MD -MP -MF compiles (rc=$df_rc)"

grep -q '^t.o:' "$DW/t.d" 2>/dev/null
check "$?" "the depfile names the object as its target (t.o:)"

# The FULL path, anchored as a whole token. `grep -F t.h` cannot fail here:
# the fixture includes <axl.h> and 40-odd SDK headers contain that substring
# (axl-atexit.h, axl-list.h, axl-format.h, ...).
dep_tokens "$DW/t.d" | grep -qx 't.h'
check "$?" "the local header is listed as its own prerequisite"

# THE DISTINCTION THAT MATTERS. The SDK arrives via -isystem, so -MMD -- which
# omits system headers by definition -- lists NONE of it. A consumer choosing
# -MMD gets zero SDK dependency tracking, which is silent staleness, not merely
# awkward paths.
( cd "$DW" && "$AXL_CC" -c -MMD -MF t-mmd.d t.c -o t-mmd.o ) >"$DW/mmd.log" 2>&1
mmd_rc=$?
mmd_sdk=$(dep_tokens "$DW/t-mmd.d" | grep -c 'include/axl-sdk/')
[[ "$mmd_rc" -eq 0 && -s "$DW/t-mmd.d" && "$mmd_sdk" -eq 0 ]]
check "$?" "-MMD lists NO SDK header — they arrive via -isystem (found $mmd_sdk)"

md_sdk=$(dep_tokens "$DW/t.d" | grep -c 'include/axl-sdk/')
[[ "$md_sdk" -gt 0 ]]
check "$?" "-MD DOES list them (found $md_sdk) — this is why the package uses it"

# An ABSOLUTE source, which is what the CMake package passes: gcc's output is
# then all-absolute with no post-processing, which is the whole reason
# --depfile could be deleted.
( cd "$DW" && "$AXL_CC" -c -MD -MF abs.d "$DW/t.c" -o abs.o ) >"$DW/abs.log" 2>&1
abs_rc=$?
abs_rel=$(dep_tokens "$DW/abs.d" | grep -cvE '^/')
abs_abs=$(dep_tokens "$DW/abs.d" | grep -cE '^/')
# The positive control matters as much as the count: a missing or empty .d has
# zero relative tokens too, and would pass this vacuously.
[[ "$abs_rc" -eq 0 && -s "$DW/abs.d" && "$abs_abs" -gt 0 && "$abs_rel" -eq 0 ]]
check "$?" "an ABSOLUTE source yields an all-absolute depfile ($abs_abs abs, $abs_rel rel)"

# 6. Bit-parity: dependency flags must not perturb the object. gcc documents
#    them as codegen-neutral; this is what makes an incremental build's object
#    comparable to a clean one.
( cd "$DW" && "$AXL_CC" -c t.c -o t-nodep.o ) >"$DW/nodep.log" 2>&1
cmp -s "$DW/t.o" "$DW/t-nodep.o"
check "$?" "-MD -MP -MF leaves the object bit-identical to a bare compile"

# 7. The removed flag is REJECTED, not silently ignored. axl-cc errors on
#    unknown --options, so this also pins that --depfile really is gone rather
#    than lingering as a no-op nobody noticed.
"$AXL_CC" -c --depfile "$DW/x.d" "$DW/t.c" -o "$DW/x.o" >"$DW/gone.log" 2>&1
gone_rc=$?
[[ "$gone_rc" -ne 0 ]] && grep -qi "unknown option: --depfile" "$DW/gone.log"
check "$?" "--depfile is gone and rejected as an unknown option (rc=$gone_rc)"

# 8. Asking for a depfile on a LINK invocation says so -- in BOTH spellings,
#    which fail differently.
#
# `axl-cc -MD ... -o app.efi` used to be an error while --depfile existed
# ("requires -c"). Forwarded, gcc accepts it, and what you get is unusable
# either way:
#
#   with -MF     the file IS written -- but its target names the scratch
#                object in $TMPDIR that axl-cc deletes, so no make rule will
#                ever match it. That is worse than absent: it looks fine.
#   without -MF  gcc derives the path from that same scratch object, so the
#                file lands in $TMPDIR and vanishes with it.
#
# The first version of this warning fired ONLY on -MF and claimed no file was
# written -- exactly backwards on both counts, caught by review. So these
# assert the SUBSTANCE (what is on disk, what the target line says) and not
# merely that some warning text appeared.
#
# A warning and not an error: -MD in a shared CFLAGS reaching both the compile
# and the link step is an ordinary Makefile pattern.
# Its own source with a main(): t.c above is a helper TU, and a link needs an
# entry point. -I"$DW" because this compiles from the test's cwd, not from $DW.
# Includes t.h (so the dependency machinery is exercised) but does not CALL
# into it -- only this one TU is linked, so a call would be undefined.
printf '#include "t.h"\n#include <axl.h>\nint main(void){ return 0; }\n' \
    > "$DW/lmain.c"
"$AXL_CC" -I"$DW" -MD -MF "$DW/link.d" "$DW/lmain.c" -o "$WORK/link.efi" \
    >"$DW/link.log" 2>&1
lk_rc=$?
[[ "$lk_rc" -eq 0 ]] && grep -qi "LINK invocation" "$DW/link.log"
check "$?" "-MD -MF on a LINK invocation warns (rc=$lk_rc)"

# The substance: the file exists and its target is a path that no longer does.
if [[ -s "$DW/link.d" ]]; then
    lk_tgt="$(sed -n '1s/:.*//p' "$DW/link.d")"
    [[ -n "$lk_tgt" && ! -e "$lk_tgt" ]]
    check "$?" "...and its target names an object that no longer exists ($lk_tgt)"
else
    check 1 "...and its target names an object that no longer exists (no depfile)"
fi

# The other spelling, which was the genuinely silent one.
( cd "$DW" && rm -f nomf.d && "$AXL_CC" -I"$DW" -MD lmain.c -o "$WORK/nomf.efi" ) \
    >"$DW/nomf.log" 2>&1
nf_rc=$?
[[ "$nf_rc" -eq 0 ]] && grep -qi "LINK invocation" "$DW/nomf.log"
check "$?" "-MD WITHOUT -MF also warns — it wrote nothing at all (rc=$nf_rc)"

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

# The C++ cases need a working C++ toolchain -- a bare-metal cross on both
# arches now (ours for x64, ARM's for aa64). Probe once by compiling, rather than guessing
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

# --- Constructors that nothing would run ------------------------------------
#
# AXL walks .init_array only. The linker scripts also bound the legacy .ctors
# with __CTOR_LIST__/__CTOR_END__, and nothing reads those -- so a non-empty
# .ctors is code the author expects to execute and that silently will not.
#
# THE REACHABLE CAUSE IS A COMPILER. GCC's x86_64-*-elf target defaults to
# .ctors; AXL's own toolchain escapes it only via --enable-initfini-array
# (14.3.0-axl2). An AXL_X64_GXX pointing at an older build -- a stale override,
# a warm CI cache -- silently disables every global constructor in the image.
#
# SYNTHESIZED IN ASSEMBLY rather than by invoking such a compiler, deliberately:
# a test that needs the OLD toolchain installed would SKIP everywhere it
# matters, including CI. Ten lines of .S reproduce the exact condition the
# guard inspects, on any machine.
# $WORK, not $DW: $DW belongs to the --depfile subtest above, and nesting
# here would make this block depend on that one still existing.
CT="$WORK/ctors"; mkdir -p "$CT"
cat > "$CT/ctors.S" <<'EOS'
    .text
    .globl axl_test_ctor_fn
axl_test_ctor_fn:
    ret
    .section .ctors,"aw",@progbits
    .align 8
    .quad axl_test_ctor_fn
EOS
printf '#include <axl.h>\nint main(void){ axl_print("hi\\n"); return 0; }\n' \
    > "$CT/main.c"

if "$AXL_CC" -c "$CT/ctors.S" -o "$CT/ctors.o" >"$CT/asm.log" 2>&1; then
    "$AXL_CC" "$CT/main.c" "$CT/ctors.o" -o "$CT/bad.efi" >"$CT/link.log" 2>&1
    ct_rc=$?
    [[ "$ct_rc" -ne 0 ]] && grep -q "constructors in .ctors" "$CT/link.log"
    check "$?" "an image with unwalked .ctors is REJECTED (rc=$ct_rc)"

    [[ ! -f "$CT/bad.efi" ]]
    check "$?" "the rejected link wrote no .efi"

    # The control. Without it, a guard that rejected EVERY link would pass the
    # assertion above -- and the .ctors bounds are emitted into every image, so
    # an off-by-one comparison would do exactly that.
    "$AXL_CC" "$CT/main.c" -o "$CT/good.efi" >"$CT/good.log" 2>&1
    check "$?" "the same source WITHOUT the .ctors object still links"
else
    check 1 "axl-cc assembles a .S fixture (see $CT/asm.log)"
fi

# --- Hermeticity: no host headers, no host libraries -------------------------
#
# A UEFI image cannot use either -- they describe another libc, another ABI and
# an OS that will not be there -- and the failure is not a link error, it is a
# struct that disagrees at runtime. Two checks, because neither covers the
# other:
#
#   FLAGS    fire on intent, before anything compiles, so the message names the
#            flag rather than a header six includes down. This is also the ONLY
#            one that can see `-L/usr/lib`: a depfile lists headers.
#   -MD      the compiler's own record of every file it OPENED. Catches an
#            absolute `#include "/usr/..."`, which names no flag at all.
#            -MD and not -MMD: -MMD omits SYSTEM headers by definition, so a
#            leak through `-isystem /usr/include` is invisible to it (measured:
#            -MMD 0 hits, -MD 19, same TU).
HM="$WORK/herm"; mkdir -p "$HM"
printf '#include <axl.h>\nint main(void){ return 0; }\n' > "$HM/clean.c"
printf '#include <axl.h>\n#include "/usr/include/linux/limits.h"\nint main(void){ return PATH_MAX; }\n' \
    > "$HM/absinc.c"

"$AXL_CC" -I/usr/include "$HM/clean.c" -o "$HM/a.efi" >"$HM/inc.log" 2>&1
hm_rc=$?
[[ "$hm_rc" -ne 0 ]] && grep -q "names a HOST path" "$HM/inc.log"
check "$?" "-I/usr/include is rejected before compiling (rc=$hm_rc)"

"$AXL_CC" -Wl,-L/usr/lib "$HM/clean.c" -o "$HM/b.efi" >"$HM/lib.log" 2>&1
hm_rc=$?
[[ "$hm_rc" -ne 0 ]] && grep -q "names a HOST path" "$HM/lib.log"
check "$?" "-Wl,-L/usr/lib is rejected — no header check could see it (rc=$hm_rc)"

# The case ONLY the -MD scan catches: the source names the path itself.
if [[ -r /usr/include/linux/limits.h ]]; then
    "$AXL_CC" "$HM/absinc.c" -o "$HM/c.efi" >"$HM/abs.log" 2>&1
    hm_rc=$?
    [[ "$hm_rc" -ne 0 ]] && grep -q "reached the HOST's headers" "$HM/abs.log"
    check "$?" "an absolute #include of a host header is rejected (rc=$hm_rc)"

    grep -q "/usr/include/linux/limits.h" "$HM/abs.log"
    check "$?" "...and the offending header is named, not just the source"
else
    check 0 "SKIP: /usr/include/linux/limits.h absent — absolute-include case not run"
    check 0 "SKIP: /usr/include/linux/limits.h absent — header-naming case not run"
fi

# The two regressions the FIRST version of these checks shipped, both found by
# review rather than by the tests above.
#
#   1. A leak behind a preprocessor guard. The -M pass omitted the real
#      compile's -D flags, so a host include inside `#ifdef NDEBUG` was opened
#      by a --release build and invisible to the check.
#   2. A host archive handed in POSITIONALLY. No flag scan and no depfile can
#      see it, and it is the more dangerous direction: a glibc object linked
#      straight into a firmware image.
if [[ -r /usr/include/linux/limits.h ]]; then
    printf '#include <axl.h>\n#ifdef NDEBUG\n#include "/usr/include/linux/limits.h"\n#endif\nint main(void){ return 0; }\n' \
        > "$HM/guarded.c"
    "$AXL_CC" --release "$HM/guarded.c" -o "$HM/g.efi" >"$HM/guard.log" 2>&1
    hm_rc=$?
    [[ "$hm_rc" -ne 0 ]] && grep -q "reached the HOST's headers" "$HM/guard.log"
    check "$?" "a host header behind #ifdef NDEBUG is still caught (rc=$hm_rc)"
else
    check 0 "SKIP: /usr/include/linux/limits.h absent — #ifdef-guarded case not run"
fi

if [[ -r /usr/lib64/libm.a || -r /usr/lib/x86_64-linux-gnu/libm.a ]]; then
    hostlib=/usr/lib64/libm.a
    [[ -r "$hostlib" ]] || hostlib=/usr/lib/x86_64-linux-gnu/libm.a
    "$AXL_CC" "$HM/clean.c" "$hostlib" -o "$HM/e.efi" >"$HM/poslib.log" 2>&1
    hm_rc=$?
    [[ "$hm_rc" -ne 0 ]] && grep -q "names a HOST path" "$HM/poslib.log"
    check "$?" "a host .a passed positionally is rejected (rc=$hm_rc)"
else
    check 0 "SKIP: no host libm.a — positional-archive case not run"
fi

# THE PACKAGED-INSTALL CASE, which is why the exemption is on the SDK's include
# DIR and not on $SDK_DIR. build-packages.sh stages with --prefix /usr, so a
# .deb consumer's own headers sit at /usr/include/axl-sdk/. Exempting the
# PREFIX there would whitelist the entire host tree and silently disable this
# check exactly where it ships; exempting the prefix was in fact the first fix
# attempted, and this is what caught it.
sdk_inc_exempt=$(printf '%s\n' /usr/include/axl-sdk/axl.h /usr/include/stdio.h \
    | grep -E '^(/usr/|/lib/)' | grep -vc "^/usr/include/axl-sdk/")
[[ "$sdk_inc_exempt" -eq 1 ]]
check "$?" "with SDK_DIR=/usr the exemption spares axl-sdk/ and NOT stdio.h"

# The control. Without it a check that rejected EVERYTHING would pass above.
"$AXL_CC" "$HM/clean.c" -o "$HM/ok.efi" >"$HM/ok.log" 2>&1
check "$?" "a build naming no host path still succeeds"

# The opt-out is documented, so it must work — otherwise the error message
# tells the user to do something that fails.
"$AXL_CC" --allow-host-paths -I/usr/include "$HM/clean.c" -o "$HM/d.efi" \
    >"$HM/opt.log" 2>&1
check "$?" "--allow-host-paths permits it, as the error message promises"

echo "--- results ---"
echo "axl-cc flag passthrough: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
