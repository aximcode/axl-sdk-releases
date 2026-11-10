#!/bin/bash
# test-discover-selftest.sh — host-only checks for lib/discover.sh (no QEMU).
# Not a QEMU integration test; excluded from discovery by name.
set -euo pipefail
cd "$(dirname "$0")"
source lib/discover.sh
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source ../../scripts/axl-cmake-pin.sh
axl_use_pinned_cmake || exit 1

fail=0
check() {
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1 (got '$2' want '$3')"; fail=1
    fi
}

# Fixture script with a full meta tag.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test-fixture-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=both needs=swtpm,openssl est=42 local-only=1
EOF
check "arch field"        "$(test_meta_field "$tmp/test-fixture-qemu.sh" arch)"       "both"
check "needs field"       "$(test_meta_field "$tmp/test-fixture-qemu.sh" needs)"      "swtpm,openssl"
check "est field"         "$(test_meta_field "$tmp/test-fixture-qemu.sh" est)"        "42"
check "local-only field"  "$(test_meta_field "$tmp/test-fixture-qemu.sh" local-only)" "1"

# A script with no meta tag gets defaults.
cat > "$tmp/test-bare-qemu.sh" <<'EOF'
#!/bin/bash
echo hi
EOF
check "default arch"       "$(test_meta_field "$tmp/test-bare-qemu.sh" arch)"       "x64"
check "default est"        "$(test_meta_field "$tmp/test-bare-qemu.sh" est)"        "20"
check "default local-only" "$(test_meta_field "$tmp/test-bare-qemu.sh" local-only)" "0"
check "default builds"     "$(test_meta_field "$tmp/test-bare-qemu.sh" builds)"     ""

# --- builds=: the targets a test builds ITSELF -------------------------------
#
# The runner pre-builds these BEFORE it decides the cache, because the cache
# key hashes the staged artifacts and a fixture the test has not built yet
# hashes as its previous self (lib/test-cache.sh). A declaration nothing
# reads would be worse than none, so the field is parsed here, consumed by
# run-integration.sh, and cross-checked against the script's own `make` line
# by `--lint` below.

# A field may live on a SECOND `# test-meta:` line: builds= for a test with
# nine fixtures is longer than the rest of the header put together, and one
# 200-column line is how a header stops being read.
cat > "$tmp/test-two-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=x64 est=9 local-only=0
# test-meta: builds=alpha-fixture,beta-fixture
EOF
check "second meta line: arch still read" "$(test_meta_field "$tmp/test-two-qemu.sh" arch)"   "x64"
check "second meta line: builds read"     "$(test_meta_field "$tmp/test-two-qemu.sh" builds)" "alpha-fixture,beta-fixture"

# test_meta_builds splits the field into one make argument per line, so no
# caller has to re-learn the comma convention (or the VAR=VALUE entries).
cat > "$tmp/test-vars-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=both est=9 builds=AXL_CPP=1,cxx-thing-selftest
EOF
check "builds splits on commas, keeping VAR=VALUE whole" \
    "$(test_meta_builds "$tmp/test-vars-qemu.sh" | paste -sd'|' -)" \
    "AXL_CPP=1|cxx-thing-selftest"
check "no builds= yields nothing" \
    "$(test_meta_builds "$tmp/test-bare-qemu.sh" | wc -l | tr -d ' ')" "0"

# --- arch=: an unrecognised value must be REFUSED, not run everywhere --------
#
# `discover_tests`' case had no default arm, so `arch=X64` (uppercase) fell
# through every branch and the test ran on BOTH arches -- three tests were in
# that state. A gate that cannot see a typo in its own metadata is the theme
# of this whole change.
cat > "$tmp/test-badarch-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=X64 est=1
EOF
# Driven through the lint, which is where a human sees it. The status is
# captured with `|| rc=$?`, never a bare assignment: `set -e` is in force
# here and `o=$(cmd-that-fails)` kills the script before the next line runs
# -- which is exactly how this block silently truncated the run on its first
# draft, reporting 12 PASSes and exit 1 with no FAIL in sight.
lintdir_arch=$(mktemp -d)
cp "$tmp/test-badarch-qemu.sh" "$lintdir_arch/test-badarch-qemu.sh"
rc=0
o=$(TEST_LINT_DIR="$lintdir_arch" bash lib/discover.sh --lint 2>&1) || rc=$?
if [[ $rc -ne 0 ]] && grep -qF 'test-badarch-qemu.sh: arch=X64 is not one of x64, aa64, both, none' <<<"$o"; then
    echo "  PASS: an unrecognised arch= fails the lint by name"
else
    echo "  FAIL: an unrecognised arch= did not fail the lint by name (rc=$rc): $o"; fail=1
fi
# ...and the four recognised values do not. `none` is a real value here --
# 25 host-only tests declare it, meaning "no arch of its own" -- so a
# refusal that swept it up would drop a quarter of the suite.
for _a in x64 aa64 both none; do
    printf '#!/bin/bash\n# test-meta: arch=%s est=1\n' "$_a" > "$lintdir_arch/test-badarch-qemu.sh"
    TEST_LINT_DIR="$lintdir_arch" bash lib/discover.sh --lint >/dev/null 2>&1 \
        || { echo "  FAIL: arch=$_a was rejected by the lint"; fail=1; }
done
rm -rf "$lintdir_arch"
# The value itself is still READ as written -- discovery does not silently
# rewrite it to something it understands. What stops the typo is the lint
# above, not a filter that would drop the test and its coverage without
# saying so.
check "an unrecognised arch= is read as written, not normalised" \
    "$(test_meta_field "$tmp/test-badarch-qemu.sh" arch)" "X64"
check "test_meta_arch_bad names the offending value" \
    "$(test_meta_arch_bad "$tmp/test-badarch-qemu.sh")" "X64"
check "test_meta_arch_bad is silent on a good one" \
    "$(test_meta_arch_bad "$tmp/test-fixture-qemu.sh")" ""

# --- the drift gate: builds= must match the script's own build invocation ----
#
# THE DECLARATION AND THE BUILD LINE ARE TWO COPIES OF ONE FACT, and the
# whole point of the field is that the runner builds what the test builds. A
# test that grows a second fixture and forgets the header would put itself
# straight back into the stale-key hole this closes, silently.
lintdir=$(mktemp -d); trap 'rm -rf "$tmp" "$lintdir"' EXIT
mkdir -p "$lintdir/lib"
lint_out() {   # $1 = script body; echoes the lint output plus rc=N
    printf '%s\n' "$1" > "$lintdir/test-probe-qemu.sh"
    local o; o=$(TEST_LINT_DIR="$lintdir" bash lib/discover.sh --lint 2>&1); local r=$?
    printf '%s\nrc=%s\n' "$o" "$r"
}
ok=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$ok" \
    && echo "  PASS: a matching builds= declaration passes the lint" \
    || { echo "  FAIL: a matching builds= declaration failed the lint: $ok"; fail=1; }

und=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$und" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$und" \
    && echo "  PASS: an undeclared fixture target fails the lint by name" \
    || { echo "  FAIL: an undeclared fixture target did not fail the lint by name: $und"; fail=1; }

# The same two cases through scripts/axl-build.sh, the build-system-neutral
# spelling of the same make line (slice 3 moves every caller to it). Read as
# the make line it stands for: a reader that skipped it would report every
# migrated test clean while it built undeclared fixtures.
wok=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$wok" \
    && echo "  PASS: axl-build.sh: a matching builds= declaration passes the lint" \
    || { echo "  FAIL: axl-build.sh: a matching builds= declaration failed the lint: $wok"; fail=1; }
wund=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
out=$("$PROJECT_DIR/scripts/axl-build.sh" -s ARCH="$_a" probe-fixture)')
grep -q '^rc=1$' <<<"$wund" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$wund" \
    && echo "  PASS: axl-build.sh: an undeclared fixture target fails the lint by name" \
    || { echo "  FAIL: axl-build.sh: an undeclared fixture target did not fail the lint by name: $wund"; fail=1; }
wpp=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
P=$("$PROJECT_DIR/scripts/axl-build.sh" --print-prefix --abs ARCH=x64)')
grep -q '^rc=0$' <<<"$wpp" \
    && echo "  PASS: axl-build.sh --print-prefix builds nothing and needs no declaration" \
    || { echo "  FAIL: axl-build.sh --print-prefix was read as a build: $wpp"; fail=1; }

stale=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture,gone-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$stale" && grep -qF 'test-probe-qemu.sh declares gone-fixture but its build line does not build it' <<<"$stale" \
    && echo "  PASS: a stale builds= entry fails the lint by name" \
    || { echo "  FAIL: a stale builds= entry did not fail the lint by name: $stale"; fail=1; }

# build-backend= named make or cmake until the Makefile was retired: a test
# still declaring it is refused by name, and one that RUNS make on this tree
# too -- it builds nothing now, or some other make project in its cwd.
bbret=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 build-backend=cmake builds=probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$bbret" && grep -qxF 'test-probe-qemu.sh: build-backend= is retired with the Makefile (5.0.0); remove it' <<<"$bbret" \
    && echo "  PASS: a build-backend= is refused by name" \
    || { echo "  FAIL: a build-backend= was not refused by name: $bbret"; fail=1; }
for _spec in 'make -C:make -C "$PROJECT_DIR" ARCH=x64 probe-fixture' \
             'a bare make:make probe-fixture ARCH=x64' \
             'a make behind timeout:timeout 300 make probe-fixture' \
             'a make after then:if true; then make probe-fixture; fi' \
             'a make behind three prefix words:nice -n 19 make probe-fixture' \
             'a make by path:/usr/bin/make probe-fixture' \
             'a make in a quoted command substitution:x="$(make probe-fixture)"' \
             'a bare make before a separator:if true; then make; fi' \
             "a make between apostrophes in messages:echo \"don't\"; make probe-fixture; echo \"it's\""; do
    _why="${_spec%%:*}"; _cmd="${_spec#*:}"
    _o=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
'"$_cmd")
    grep -q '^rc=1$' <<<"$_o" && grep -qxF 'test-probe-qemu.sh runs make, which no longer builds this tree (5.0.0) -- build through scripts/axl-build.sh:' <<<"$_o" \
        && echo "  PASS: $_why is refused" \
        || { echo "  FAIL: $_why was not refused: $_o"; fail=1; }
done
# ...and a make in a lib the test SOURCES, as builds= follows them.
printf '%s\n' 'make -C "$PROJECT_DIR" ARCH=x64 probe-fixture' > "$lintdir/lib/probe-mk.sh"
mklib=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
source "$(dirname "$0")/lib/probe-mk.sh"')
grep -q '^rc=1$' <<<"$mklib" && grep -qxF 'test-probe-qemu.sh runs make, which no longer builds this tree (5.0.0) -- build through scripts/axl-build.sh:' <<<"$mklib" \
    && echo "  PASS: a make in a sourced lib is refused" \
    || { echo "  FAIL: a make in a sourced lib was not refused: $mklib"; fail=1; }
rm -f "$lintdir/lib/probe-mk.sh"
# CONTROLS: `make` that is not a make -- a word in a comment or in a quoted
# message, a cmake call -- is no refusal.
# (A heredoc, not a '...' literal: the body has single quotes of its own.)
mkctl=$(lint_out "$(cat <<'BODY'
#!/bin/bash
# test-meta: arch=x64 est=1
# make sure; make it so
echo "build it first; then make sure it exists"
fail "x (make sure y)"
echo 'then make sure'
echo "built $(date) -- then make sure it ran"
msg="x $(nproc) cpus; make it so"
echo "it is $(basename "$f") and do make sure"
echo '$(make foo)'
cmake --build "$PROJECT_DIR/build/x64-debug"
BODY
)")
grep -q '^rc=0$' <<<"$mkctl" \
    && echo "  PASS: control -- make in a comment or a quoted message, and cmake, are not make" \
    || { echo "  FAIL: control -- a comment or cmake was taken for make: $mkctl"; fail=1; }

# size-gate= is held to builds=: a gate on a target the test does not build
# caps nothing, and a typo'd target would route no change to it. CONTROL
# first, so a lint that refused every size-gate= cannot pass.
sgok=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture size-gate=probe-fixture size-gate-paths=src/
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$sgok" \
    && echo "  PASS: a size-gate= on a builds= target passes the lint" \
    || { echo "  FAIL: a size-gate= on a builds= target failed the lint: $sgok"; fail=1; }
sgbad=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture size-gate=probe-fixtrue size-gate-paths=src/
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$sgbad" && grep -qxF 'test-probe-qemu.sh size-gates probe-fixtrue, which its builds= does not build' <<<"$sgbad" \
    && echo "  PASS: a size-gate= on a target the test does not build fails the lint by name" \
    || { echo "  FAIL: a size-gate= on an unbuilt target did not fail the lint by name: $sgbad"; fail=1; }

# ...and the two halves go together: a size-gate= with no paths routes
# nothing to the gate, and paths with no target give the drift check nothing
# to derive.
sgnop=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture size-gate=probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$sgnop" && grep -qxF 'test-probe-qemu.sh size-gates probe-fixture but declares no size-gate-paths=' <<<"$sgnop" \
    && echo "  PASS: a size-gate= with no size-gate-paths= fails the lint by name" \
    || { echo "  FAIL: a size-gate= with no size-gate-paths= did not fail the lint by name: $sgnop"; fail=1; }
sgnot=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture size-gate-paths=src/
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$sgnot" && grep -qxF 'test-probe-qemu.sh declares size-gate-paths= but no size-gate= target' <<<"$sgnot" \
    && echo "  PASS: size-gate-paths= with no size-gate= fails the lint by name" \
    || { echo "  FAIL: size-gate-paths= with no size-gate= did not fail the lint by name: $sgnot"; fail=1; }
# size_gate_covers is a STRING-PREFIX match: a submodule's bare gitlink path
# is under deps/, and a prefix that is not a directory still matches its
# files (tools/sshd -> tools/sshd-svc.c). CONTROLS: paths under no prefix.
printf '%s\n' '#!/bin/bash' '# test-meta: size-gate=x size-gate-paths=deps/,tools/sshd,VERSION' > "$tmp/test-sgc-qemu.sh"
for pth in deps/mbedtls tools/sshd-svc.c VERSION; do
    size_gate_covers "$tmp/test-sgc-qemu.sh" "$pth" \
        && echo "  PASS: size_gate_covers matches $pth" \
        || { echo "  FAIL: size_gate_covers did not match $pth"; fail=1; }
done
for pth in docs/AXL-Design.md tools/9p.c; do
    if size_gate_covers "$tmp/test-sgc-qemu.sh" "$pth"; then
        echo "  FAIL: size_gate_covers matched $pth (CONTROL)"; fail=1
    else
        echo "  PASS: size_gate_covers does not match $pth"
    fi
done
# size-gate-exclude= takes a path back out of routing; `*` crosses `/`.
printf '%s\n' '#!/bin/bash' '# test-meta: size-gate=x size-gate-paths=src/ size-gate-exclude=*.md' > "$tmp/test-sgx-qemu.sh"
if size_gate_routes "$tmp/test-sgx-qemu.sh" src/loop/README.md; then
    echo "  FAIL: size_gate_routes routed src/loop/README.md past size-gate-exclude=*.md"; fail=1
else
    echo "  PASS: size_gate_routes skips src/loop/README.md (size-gate-exclude=*.md)"
fi
size_gate_routes "$tmp/test-sgx-qemu.sh" src/loop/axl-loop.c \
    && echo "  PASS: size_gate_routes still routes src/loop/axl-loop.c" \
    || { echo "  FAIL: size_gate_routes did not route src/loop/axl-loop.c (CONTROL)"; fail=1; }
sgxo=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture size-gate-exclude=*.md
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$sgxo" && grep -qxF 'test-probe-qemu.sh declares size-gate-exclude= but no size-gate= target' <<<"$sgxo" \
    && echo "  PASS: size-gate-exclude= with no size-gate= fails the lint by name" \
    || { echo "  FAIL: size-gate-exclude= with no size-gate= did not fail the lint by name: $sgxo"; fail=1; }

# A target the runner ALREADY pre-builds must not be declared -- declaring it
# would make every run relink `all` a second time for nothing.
pre=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" all tests 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$pre" \
    && echo "  PASS: the pre-built targets need no declaration" \
    || { echo "  FAIL: a build of pre-built targets was flagged: $pre"; fail=1; }

# A LINE CONTINUATION HIDES NOTHING. Most tests in the tree wrap their build
# across two lines, so a parser that read one line at a time would see no
# targets at all and report the whole suite clean.
cont=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" \
    all probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$cont" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$cont" \
    && echo "  PASS: a build split across a continuation is still read" \
    || { echo "  FAIL: a continued build line was not read: $cont"; fail=1; }

# WHAT A TEST SOURCES, IT BUILDS. Three ssh tests build their fixture only
# through lib/ssh-hostile-lib.sh; a parser that stopped at the test file
# would call them clean and leave them in the hole.
printf '%s\n' '"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" lib-fixture 2>&1 | tail -1' \
    > "$lintdir/lib/probe-lib.sh"
sourced=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
source "$(dirname "$0")/lib/probe-lib.sh"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$sourced" && grep -qF 'test-probe-qemu.sh builds lib-fixture but does not declare it' <<<"$sourced" \
    && echo "  PASS: a target built by a sourced lib must be declared too" \
    || { echo "  FAIL: a lib's build line was not attributed to the test: $sourced"; fail=1; }
dotted=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
. "$(dirname "$0")/lib/probe-lib.sh"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$dotted" && grep -qF 'test-probe-qemu.sh builds lib-fixture but does not declare it' <<<"$dotted" \
    && echo "  PASS: ...and so must one built by a lib sourced with '.'" \
    || { echo "  FAIL: a '.'-sourced lib's build line was not attributed to the test: $dotted"; fail=1; }
# ...and TRANSITIVELY: a lib that a sourced lib sources builds for the test
# too (the split ssh idle suites are wrappers over lib/ssh-idle-cases.sh,
# which reaches its fixture through lib/ssh-hostile-lib.sh). The outer lib
# also sources ITSELF, so a reader that followed sources without remembering
# what it had read would never return -- bounded here, so that is a FAIL
# rather than a hung self-test.
printf '%s\n' 'source "$(dirname "$0")/lib/probe-outer.sh"' \
    'source "$(dirname "$0")/lib/probe-lib.sh"' > "$lintdir/lib/probe-outer.sh"
printf '%s\n' '#!/bin/bash' '# test-meta: arch=x64 est=1 builds=probe-fixture' \
    'source "$(dirname "$0")/lib/probe-outer.sh"' \
    '"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1' \
    > "$lintdir/test-probe-qemu.sh"
nested=$(TEST_LINT_DIR="$lintdir" timeout 30 bash lib/discover.sh --lint 2>&1; echo "rc=$?")
grep -q '^rc=1$' <<<"$nested" && grep -qF 'test-probe-qemu.sh builds lib-fixture but does not declare it' <<<"$nested" \
    && echo "  PASS: ...and so must one built by a lib that a sourced lib sources (a self-sourcing lib still ends)" \
    || { echo "  FAIL: a nested lib's build line was not attributed to the test: $nested"; fail=1; }
rm -f "$lintdir/lib/probe-outer.sh"
# ...but ONLY what it sources. A lib a test merely NAMES -- in a comment, or
# one it EXECUTES as the subject under test (test-ctest-integration.sh runs
# lib/suite-prep.sh) -- builds nothing on the test's behalf, and attributing
# its build lines made the gate demand builds= the test never performs.
named=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
# the harness is lib/probe-lib.sh
bash "$SCRIPT_DIR/lib/probe-lib.sh" --dry
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$named" \
    && echo "  PASS: a lib only named or executed is not attributed to the test" \
    || { echo "  FAIL: a named/executed lib's build line was attributed to the test: $named"; fail=1; }
rm -f "$lintdir/lib/probe-lib.sh"

# AND THE CASE THE PARSER CANNOT READ IS A FAILURE, not a pass. An
# unresolvable word in target position is the same empty string as "this test
# builds nothing", and the two are opposite facts.
opaque=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" "$SOME_TARGET" 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$opaque" && grep -qF 'cannot tell what test-probe-qemu.sh builds' <<<"$opaque" \
    && echo "  PASS: an unreadable build argument fails the lint instead of reading as clean" \
    || { echo "  FAIL: an unreadable build argument did not fail the lint: $opaque"; fail=1; }

# A LITERAL ARCH= IS A DECLARATION TOO. test-fwtool-host.sh runs on both
# arches and builds its host tool at x64; pre-building that at the run's arch
# would build the wrong thing on the aa64 leg.
archlit=$(lint_out '#!/bin/bash
# test-meta: arch=both est=1 builds=probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 probe-fixture 2>&1 | tail -1')
grep -q '^rc=1$' <<<"$archlit" && grep -qF 'test-probe-qemu.sh builds ARCH=x64 but does not declare it' <<<"$archlit" \
    && echo "  PASS: a hard-coded ARCH= beside a fixture must be declared" \
    || { echo "  FAIL: a hard-coded ARCH= was not required in builds=: $archlit"; fail=1; }
archok=$(lint_out '#!/bin/bash
# test-meta: arch=both est=1 builds=ARCH=x64,probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 probe-fixture 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$archok" \
    && echo "  PASS: ...and declaring it satisfies the lint" \
    || { echo "  FAIL: a declared ARCH= override still failed the lint: $archok"; fail=1; }
# ...but a build of only pre-built targets needs no declaration at
# all, variables included -- two dozen tests spell `ARCH=x64 tools`, and a
# build group with no targets in it is not worth a header field.
archpre=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 tools 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$archpre" \
    && echo "  PASS: a variable on a pre-built-only build needs no declaration" \
    || { echo "  FAIL: ARCH=x64 on a tools-only build was flagged: $archpre"; fail=1; }

# A RELATIVE SPELLING COUNTS TOO. A script that `cd "$PROJECT_DIR"` first
# calls scripts/axl-build.sh by its relative path; the make-era parser once
# matched only `make -C "$PROJECT_DIR"` and reported six such scripts clean
# while two built fixtures nothing declared.
bare=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
scripts/axl-build.sh probe-fixture ARCH=x64')
grep -q '^rc=1$' <<<"$bare" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$bare" \
    && echo "  PASS: a relative scripts/axl-build.sh is read" \
    || { echo "  FAIL: a relative scripts/axl-build.sh was not read: $bare"; fail=1; }
# ...including the conditional form both offenders used, which is a worse
# case than it looks: the artifact is rebuilt only when MISSING, so after a
# library change the test does not refresh it even when it does run.
cond=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
[[ -f "$DRV" ]] || scripts/axl-build.sh probe-fixture ARCH="$_a" 2>&1 | tail -3')
grep -q '^rc=1$' <<<"$cond" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$cond" \
    && echo "  PASS: a conditional build is read" \
    || { echo "  FAIL: a conditional build was not read: $cond"; fail=1; }
# ...and one behind a COMMAND PREFIX, which is not a command position:
# lib/console-device-lib.sh once ran its make under an flock array, with
# the nine console-device fixtures hanging off exactly that line.
pfx=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
if ! "${_lock[@]}" "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture 2>&1 | tail -3; then exit 1; fi')
grep -q '^rc=1$' <<<"$pfx" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$pfx" \
    && echo "  PASS: a build behind a command prefix is read" \
    || { echo "  FAIL: a build behind a command prefix was not read: $pfx"; fail=1; }
# ...and one written after `then` on a single line, which is a command
# position the line-start anchor does not cover.
thenmake=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
if [[ ! -f "$DRV" ]]; then scripts/axl-build.sh probe-fixture ARCH=x64; fi')
grep -q '^rc=1$' <<<"$thenmake" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$thenmake" \
    && echo "  PASS: a build after then on one line is read" \
    || { echo "  FAIL: a build after then was not read: $thenmake"; fail=1; }
# PROSE IS NOT A BUILD. The parsers read whole files, and a comment that
# happens to contain a command separator would otherwise hand them a make
# invocation out of English (test_calls_make refuses one). The `;` here is the point: without the comment
# strip the anchor matches it, and "sure the fixture exists" becomes four
# undeclared targets.
prose=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
# build it by hand first; make sure the fixture exists before booting
echo hi')
grep -q '^rc=0$' <<<"$prose" \
    && echo "  PASS: the word make in a comment is not a build, nor a make" \
    || { echo "  FAIL: a comment was parsed as a make invocation: $prose"; fail=1; }
# BUILD=RELEASE IS A TREE A TEST BOOTS (test-efi-pack-release-qemu.sh stages
# the release images from it), so its build is read, and NOTHING counts as
# pre-built there: `all tools` must be declared WITH the variable, which the
# runner passes through to its fixture pre-build. Undeclared fails, naming
# the pre-built-elsewhere target; declared passes.
rel_und=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
scripts/axl-build.sh ARCH="$_a" BUILD=RELEASE all tools')
grep -q '^rc=1$' <<<"$rel_und" \
    && grep -qxF 'test-probe-qemu.sh builds all but does not declare it (add builds=... to its # test-meta: header)' <<<"$rel_und" \
    && echo "  PASS: a BUILD=RELEASE all must be declared (nothing is pre-built in that tree)" \
    || { echo "  FAIL: a BUILD=RELEASE all was not required in builds=: $rel_und"; fail=1; }
rel_ok=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=BUILD=RELEASE,all,tools
cd "$PROJECT_DIR"
scripts/axl-build.sh ARCH="$_a" BUILD=RELEASE all tools')
grep -q '^rc=0$' <<<"$rel_ok" \
    && echo "  PASS: builds=BUILD=RELEASE,all,tools declares a release build" \
    || { echo "  FAIL: a declared release build failed the lint: $rel_ok"; fail=1; }

# A TRAILING COMMENT MUST NOT EAT THE TARGET. The make-era prefix strip used a
# greedy `.*`, so it cut to the LAST `make ` on the line -- a comment
# mentioning make swallowed the real invocation and the lint reported clean.
tail1=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture   # you must make all first')
grep -q '^rc=1$' <<<"$tail1" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$tail1" \
    && echo "  PASS: a trailing comment does not swallow the real target" \
    || { echo "  FAIL: a trailing comment ate the target: $tail1"; fail=1; }
# ...and the declared form with the same comment is clean, so the fix is not
# just "always fail".
tail2=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture   # you must make all first')
grep -q '^rc=0$' <<<"$tail2" \
    && echo "  PASS: ...and a declared one with the same comment is clean" \
    || { echo "  FAIL: a trailing comment made a declared target fail: $tail2"; fail=1; }
# A COMMAND PREFIX OTHER THAN flock. `timeout 300 <build> ...` has two words
# in front of it; the make-era anchor alone did not see it.
cmdpfx=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
timeout 300 scripts/axl-build.sh probe-fixture ARCH=x64')
grep -q '^rc=1$' <<<"$cmdpfx" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$cmdpfx" \
    && echo "  PASS: a build behind timeout(1) is read" \
    || { echo "  FAIL: a build behind a two-word command prefix was not read: $cmdpfx"; fail=1; }

# THREE MORE SHAPES, all found in review AFTER the gate was called done, and
# one of them was CLAIMED as covered in a committed comment. That is the
# defect this whole change exists to close, wearing the gate's own clothes.
for _spec in 'a variable assignment prefix:env AXL_CPP=1 scripts/axl-build.sh probe-fixture ARCH=x64' \
             'a bare VAR=1 prefix:AXL_TOOLCHAIN=axl scripts/axl-build.sh probe-fixture ARCH=x64' \
             'a command substitution:out=$("$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 probe-fixture)'; do
    _why="${_spec%%:*}"; _cmd="${_spec#*:}"
    _o=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
cd "$PROJECT_DIR"
'"$_cmd")
    grep -q '^rc=1$' <<<"$_o" && grep -qF 'test-probe-qemu.sh builds probe-fixture but does not declare it' <<<"$_o" \
        && echo "  PASS: $_why is read" \
        || { echo "  FAIL: $_why was not read: $_o"; fail=1; }
done
# ...and declaring it satisfies the lint, so the three are not just "always
# fail" on a shape the parser mangles.
subok=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1 builds=probe-fixture
cd "$PROJECT_DIR"
out=$("$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" probe-fixture)')
grep -q '^rc=0$' <<<"$subok" \
    && echo "  PASS: ...and a declared command substitution is clean" \
    || { echo "  FAIL: a declared command substitution still failed: $subok"; fail=1; }

# --- test_meta_field reads the HEADER, not the whole file --------------------
#
# Dropping `grep -m1` to allow a second header line widened the scan to every
# `^# test-meta:` line in the file, and it fails OPEN: a fixture script
# written into a heredoc supplies a ghost `local-only=1` and the test
# silently leaves the CI selection. Scoped to the contiguous comment block
# at the top instead.
cat > "$tmp/test-ghost-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=both est=9
cat > /tmp/inner.sh <<'INNER'
# test-meta: arch=x64 local-only=1 builds=ghost-fixture
INNER
EOF
check "a heredoc test-meta does not set local-only" \
    "$(test_meta_field "$tmp/test-ghost-qemu.sh" local-only)" "0"
check "a heredoc test-meta does not override arch" \
    "$(test_meta_field "$tmp/test-ghost-qemu.sh" arch)" "both"
check "a heredoc test-meta supplies no ghost builds=" \
    "$(test_meta_field "$tmp/test-ghost-qemu.sh" builds)" ""

# A field name must match a WHOLE field, not a substring of another value.
cat > "$tmp/test-sub-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=x64 builds=manifest=1,widget-fixture est=42
EOF
check "field= is not matched inside another field value" \
    "$(test_meta_field "$tmp/test-sub-qemu.sh" est)" "42"

# ...except a path under the pre-built tools/ tree, which four tests build by
# FILE rather than by target name. `tools` is pre-built, so those are covered.
# The substitution carries a SPACE, as every real instance does
# (`"$("$PROJECT_DIR/scripts/build-prefix.sh" "$arch")/tools/cut.efi"`).
# Without one the word survives a naive whitespace tokenizer and the case
# passes whether or not the collapse exists -- which is how a sabotage of
# the collapse came back UNDETECTED.
toolpath=$(lint_out '#!/bin/bash
# test-meta: arch=x64 est=1
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" "$("$PROJECT_DIR/scripts/build-prefix.sh" "$_a")/tools/cut.efi" 2>&1 | tail -1')
grep -q '^rc=0$' <<<"$toolpath" \
    && echo "  PASS: a built-by-path tool under the pre-built tools/ tree needs no declaration" \
    || { echo "  FAIL: a tools/ path target was flagged: $toolpath"; fail=1; }

# --- guest=: a test that runs on a pooled guest ------------------------------
#
# guest=<class> means `run-integration.sh --pool` runs it under `axlpool
# run`. That only works for a test whose commands are a checked-in
# <TestName>.nsh it runs through test_use_nsh, on a class that exists; each
# way of getting that wrong fails the lint by name rather than at run time.
printf '[class.standard]\n' > "$lintdir/classes.toml"
guest_lint() {   # $1 = script body, $2 = 1 to give it a .nsh -- lint output + rc
    rm -f "$lintdir/test-probe-qemu.nsh"
    [[ "$2" == 1 ]] && printf 'echo hi\n' > "$lintdir/test-probe-qemu.nsh"
    printf '%s\n' "$1" > "$lintdir/test-probe-qemu.sh"
    local o; o=$(TEST_LINT_DIR="$lintdir" TEST_LINT_CLASSES="$lintdir/classes.toml" \
                 bash lib/discover.sh --lint 2>&1); local r=$?
    printf '%s\nrc=%s\n' "$o" "$r"
}
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
test_use_nsh' 1)
check "guest=: a class, a .nsh and test_use_nsh pass the lint" "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=nope
test_use_nsh' 1)
check "guest=: an unknown class fails, named" "$(grep -c 'guest=nope is not a class in' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
test_use_nsh' 0)
check "guest=: no <TestName>.nsh beside it fails" "$(grep -c 'guest=standard but no test-probe-qemu.nsh' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
echo hi | test_set_startup' 1)
check "guest=: a test that runs no guest the pool can take (an inline startup.nsh) fails" \
    "$(grep -cx 'test-probe-qemu.sh: guest=standard but runs no guest the pool can take (no test_use_nsh, no call reaching test_use_nsh_text, no scripts/run-qemu.sh call)' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
# run-qemu.sh's pooled mode (design §6.9) is the other way onto a lease: a
# test whose guests are run-qemu.sh calls needs no .nsh of its own.
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
"$PROJECT_DIR/scripts/run-qemu.sh" --bridges --net app.efi' 0)
check "guest=: a test whose guests are run-qemu.sh calls passes, no .nsh needed" "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
# (scripts/run-qemu.sh named only in a comment)
echo hi | test_set_startup' 0)
check "guest=: run-qemu.sh named only in a comment does not count" \
    "$(grep -c 'guest=standard but runs no guest the pool can take' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
echo "Checking run-qemu.sh output..."' 0)
check "guest=: run-qemu.sh named in a message, not run, does not count" \
    "$(grep -c 'guest=standard but runs no guest the pool can take' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
"$RUN_QEMU" --nsh x.nsh app.efi' 0)
check "guest=: a call through a RUN_QEMU variable counts" "$(tail -1 <<<"$g")" "rc=0"
# A GENERATED script (test_use_nsh_text) is the third way: in a function of
# a lib/*.sh the test sources -- transitively -- and that the test or its libs
# CALL (lib/ssh-hostile-lib.sh's hostile_boot). Sourcing the lib for its other
# helpers while booting some other way does not count.
mkdir -p "$lintdir/lib"
printf '%s\n' 'gen_boot() {' '    inner() { :; }     # a helper defined inside: must not take the call' '    test_use_nsh_text gen << EOF' 'echo hi' 'EOF' '}' 'other_helper() { :; }' \
    > "$lintdir/lib/gen-lib.sh"
printf '%s\n' 'source "$(dirname "$0")/lib/gen-lib.sh"' 'gen_boot' > "$lintdir/lib/gen-cases.sh"
printf '%s\n' '# test_use_nsh_text named only in a comment' 'quiet_boot() { :; }' \
    > "$lintdir/lib/quiet-lib.sh"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
source "$(dirname "$0")/lib/gen-lib.sh"
gen_boot "banner"' 0)
check "guest=: a call to a sourced lib function that generates the script passes" "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
source "$(dirname "$0")/lib/gen-cases.sh"' 0)
check "guest=: the generating call two libs down (test -> cases lib -> lib) passes" "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
source "$(dirname "$0")/lib/gen-lib.sh"
other_helper
echo hi | test_set_startup' 0)
check "guest=: sourcing the generating lib WITHOUT calling its generator fails" \
    "$(grep -cx 'test-probe-qemu.sh: guest=standard but runs no guest the pool can take (no test_use_nsh, no call reaching test_use_nsh_text, no scripts/run-qemu.sh call)' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
source "$(dirname "$0")/lib/quiet-lib.sh"
quiet_boot' 0)
check "guest=: test_use_nsh_text named only in a lib comment does not count" \
    "$(grep -c 'guest=standard but runs no guest the pool can take' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
# A column-0 `}` inside a HEREDOC (an embedded sh script) is not the end of
# the function: hostile_boot writes such a script before its generator call.
printf '%s\n' 'heredoc_boot() {' '    cat > x.sh << '"'"'SH'"'"'' 'f() {' 'echo' '}' 'SH' \
    '    test_use_nsh_text gen << EOF' 'echo hi' 'EOF' '}' 'helper() { :; }' > "$lintdir/lib/heredoc-lib.sh"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
source "$(dirname "$0")/lib/heredoc-lib.sh"
helper
echo hi | test_set_startup' 0)
check "guest=: a column-0 } in a heredoc does not end the function (generator not called: fails)" \
    "$(grep -c 'guest=standard but runs no guest the pool can take' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
source "$(dirname "$0")/lib/heredoc-lib.sh"
heredoc_boot' 0)
check "guest=: ... and the same lib passes when the generator IS called" "$(tail -1 <<<"$g")" "rc=0"

# --- every guest is accounted for: a class, or guest=none with its reason ----
#
# A test that launches a guest and declares no guest= would stay booted with
# nobody having decided so. The lint names it; `guest=none` says the boot is
# deliberate, and must carry its reason as a `# guest=none: <why>` comment.
# Each launcher counts, in the test or in a lib/*.sh it sources; a launcher
# named only in a comment, or a host-only test, is not a finding (controls).
noguest='launches a guest but declares no guest= (a pool class, or guest=none with a "# guest=none: <why>" comment)'
for _l in 'test_run_foreground' 'test_run_background' 'test_timing_begin fg' \
          '"$PROJECT_DIR/scripts/run-qemu.sh" app.efi' '"$RUN_QEMU" app.efi' \
          'qemu-system-x86_64 -M none' '"${TEST_QEMU_CMD[@]}"' \
          'python3 "$PROJECT_DIR/test/pool/pool-cases.py" X64' \
          '"$TEST_QEMU_BIN" -M q35' '"$QEMU_BIN" -M q35' \
          'DEMO="$PROJECT_DIR/scripts/ssh-demo.sh"' '"$PROJECT_DIR/scripts/axl-emulate" app.efi'; do
    g=$(guest_lint "#!/bin/bash
# test-meta: arch=x64 est=1
$_l" 0)
    check "no guest=: a test launching through [$_l] fails, named" \
        "$(grep -cxF "test-probe-qemu.sh: $noguest" <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
done
printf '%s\n' 'boot_it() {' '    test_run_foreground' '}' > "$lintdir/lib/boot-lib.sh"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1
source "$(dirname "$0")/lib/boot-lib.sh"
boot_it' 0)
check "no guest=: a launch inside a sourced lib/*.sh fails, named" \
    "$(grep -cxF "test-probe-qemu.sh: $noguest" <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1
source "$SCRIPT_DIR/lib/boot-lib.sh"
boot_it' 0)
check "no guest=: a lib sourced through \$SCRIPT_DIR/lib/ is followed too" \
    "$(grep -cxF "test-probe-qemu.sh: $noguest" <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
cp lib/discover.sh "$lintdir/lib/discover.sh"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1
source "$(dirname "$0")/lib/discover.sh"
test_meta_field x arch' 0)
check "control: sourcing lib/discover.sh (whose RE names every launcher) is no launch" \
    "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1
# test_run_foreground and scripts/run-qemu.sh, named only in a comment
scripts/axl-build.sh --print-prefix' 0)
check "control: a host-only test (launchers named only in comments) passes with no guest=" \
    "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1
printf "x\n" > "$FIX/../../scripts/run-qemu.sh"
sha256sum "$dir/../../scripts/run-qemu.sh"
for want in Makefile scripts/run-qemu.sh; do :; done
echo "no scripts/run-qemu.sh call"' 0)
check "control: run-qemu.sh written, hashed, listed or named in a message is no launch" \
    "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=none
# guest=none: the boot path is the subject
test_run_foreground' 0)
check "guest=none with its reason passes" "$(tail -1 <<<"$g")" "rc=0"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=none
test_run_foreground' 0)
check "guest=none with no reason fails, named" \
    "$(grep -cxF 'test-probe-qemu.sh: guest=none but gives no reason (add a "# guest=none: <why>" comment)' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=none
# guest=none:
test_run_foreground' 0)
check "guest=none with an EMPTY reason fails, named" \
    "$(grep -cxF 'test-probe-qemu.sh: guest=none but gives no reason (add a "# guest=none: <why>" comment)' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=none
# guest=none: stale -- it no longer boots
scripts/axl-build.sh --print-prefix' 0)
check "guest=none on a test that launches no guest fails, named (stale)" \
    "$(grep -cxF 'test-probe-qemu.sh: guest=none but launches no guest (drop it)' <<<"$g") $(tail -1 <<<"$g")" "1 rc=1"
g=$(guest_lint '#!/bin/bash
# test-meta: arch=x64 est=1 guest=none
# guest=none: the boot path is the subject
test_run_foreground' 0)
check "guest=none is not looked up as a class" "$(grep -c 'is not a class' <<<"$g")" "0"
rm -rf "$lintdir/lib"
rm -f "$lintdir/test-probe-qemu.nsh" "$lintdir/classes.toml"

# test_port now lives in test-portalloc-selftest.sh, together with the
# allocator it is built on. The old "default base (standalone) == 18000"
# check went with it: a standalone run no longer has a fixed base, it claims
# a verified-free one. TEST_PORT_BASE, when set explicitly, is still exact
# arithmetic — that contract is asserted there.

# An EMPTY directory discovers nothing. The glob `test-*.sh` matches no file
# there and bash passes it through unexpanded, which once registered a CTest
# test literally named `integration.test-*` and let the est gate grade it as
# "1 test(s) fit" -- an empty selection reading as a clean one.
emptyd=$(mktemp -d); mkdir -p "$emptyd/lib"
cp lib/discover.sh "$emptyd/lib/"
check "an empty directory discovers no test" \
    "$(bash -c 'source "$1"; discover_tests --include-local-only' _ "$emptyd/lib/discover.sh" | wc -l)" "0"
check "...and --emit-ctest registers none" \
    "$(bash "$emptyd/lib/discover.sh" --emit-ctest X64 /r /p | grep -c '^add_test(\[==\[integration\.test-')" "0"
check "...and --lint over it finds no missing header" \
    "$(TEST_LINT_DIR="$emptyd" bash "$emptyd/lib/discover.sh" --lint 2>&1 | grep -c 'missing test-meta')" "0"
rm -rf "${emptyd:?}"

# AXL_DISCOVER_DIR: discover ANOTHER directory with this discover.sh -- how
# a stub directory (AXL_IT_DIR, RUN_INTEGRATION_DIR) is registered without a
# copy of lib/ of its own, which every stub used to need.
stubd=$(mktemp -d)
printf '#!/bin/bash\n# test-meta: arch=x64 est=1\nexit 0\n' > "$stubd/test-stubbed-qemu.sh"
check "AXL_DISCOVER_DIR: discover_tests lists that directory's tests" \
    "$(AXL_DISCOVER_DIR="$stubd" bash -c 'source lib/discover.sh; discover_tests --include-local-only' | xargs -n1 basename)" "test-stubbed-qemu.sh"
check "...and --emit-ctest registers them" \
    "$(AXL_DISCOVER_DIR="$stubd" bash lib/discover.sh --emit-ctest X64 /r /p | grep -c '^add_test(\[==\[integration\.test-stubbed-qemu\]==\]')" "1"
rm -rf "${stubd:?}"

# discover_tree_tests: what a TREE registers -- discover_tests, minus the
# arch=none tests on AARCH64 (they run once, in the x64 tree). The runner
# selects with it too, or an aa64 run names 46 tests its tree does not have.
treed=$(mktemp -d)
printf '#!/bin/bash\n# test-meta: arch=none est=1\nexit 0\n' > "$treed/test-host-only.sh"
printf '#!/bin/bash\n# test-meta: arch=both est=1\nexit 0\n' > "$treed/test-both-qemu.sh"
check "discover_tree_tests --arch AARCH64 leaves out arch=none" \
    "$(AXL_DISCOVER_DIR="$treed" bash -c 'source lib/discover.sh; discover_tree_tests --arch AARCH64' | xargs -n1 basename | paste -sd' ')" "test-both-qemu.sh"
check "...and --arch X64 keeps it" \
    "$(AXL_DISCOVER_DIR="$treed" bash -c 'source lib/discover.sh; discover_tree_tests --arch X64' | xargs -n1 basename | sort | paste -sd' ')" "test-both-qemu.sh test-host-only.sh"
rm -rf "${treed:?}"

# cpu=: the CPUs a test keeps busy, which ctest charges against -j (the
# PROCESSORS property), so a build-heavy test holds the slots its compilers
# use instead of a load-average gate guessing at them a minute late.
cpud=$(mktemp -d)
printf '#!/bin/bash\n# test-meta: arch=x64 est=7 cpu=3\nexit 0\n' > "$cpud/test-cpu-qemu.sh"
printf '#!/bin/bash\n# test-meta: arch=x64 est=5\nexit 0\n' > "$cpud/test-nocpu-qemu.sh"
check "cpu= defaults to 1" "$(test_meta_field "$cpud/test-nocpu-qemu.sh" cpu)" "1"
em=$(AXL_DISCOVER_DIR="$cpud" bash lib/discover.sh --emit-ctest X64 /r /p)
main_line() { grep -F "set_tests_properties([==[integration.$1]==] PROPERTIES LABELS " <<<"$em"; }
check "cpu=3 is registered as PROCESSORS 3" \
    "$(main_line test-cpu-qemu | grep -o ' PROCESSORS [0-9]* ')" " PROCESSORS 3 "
check "no cpu= is registered as PROCESSORS 1" \
    "$(main_line test-nocpu-qemu | grep -o ' PROCESSORS [0-9]* ')" " PROCESSORS 1 "
# guest=none is not a class: no guest: label, no AXL_IT_GUEST entry (which
# would make pool mode lease a class called "none"). A class is (control).
printf '#!/bin/bash\n# test-meta: arch=x64 est=1 guest=none\n# guest=none: x\nexit 0\n' > "$cpud/test-gnone-qemu.sh"
printf '#!/bin/bash\n# test-meta: arch=x64 est=1 guest=plain\nexit 0\n' > "$cpud/test-gplain-qemu.sh"
em=$(AXL_DISCOVER_DIR="$cpud" bash lib/discover.sh --emit-ctest X64 /r /p)
check "guest=none: no guest label, no AXL_IT_GUEST entry" \
    "$(main_line test-gnone-qemu | grep -c 'guest:') $(grep -c 'integration.test-gnone-qemu=' <<<"$em")" "0 0"
check "control: guest=plain is labelled and registered with its class" \
    "$(main_line test-gplain-qemu | grep -o 'guest:plain') $(grep -c 'AXL_IT_GUEST \[==\[integration.test-gplain-qemu=plain\]==\]' <<<"$em")" "guest:plain 1"
check "test_meta_pool_class: none is no class, a class is itself" \
    "[$(test_meta_pool_class "$cpud/test-gnone-qemu.sh")] [$(test_meta_pool_class "$cpud/test-gplain-qemu.sh")]" "[] [plain]"
rm -f "$cpud/test-gnone-qemu.sh" "$cpud/test-gplain-qemu.sh"
# est= is the COST only for a test the tree's cost data does not know: ctest
# ignores its own measured averages for any test with a COST property, so an
# unconditional COST would throw the measurement away on every run.
check "COST is not on the main property line" "$(main_line test-cpu-qemu | grep -c ' COST ')" "0"
check "est= is the COST only when the cost data does not name the test" \
    "$(grep -A2 -Fx 'if(NOT [==[integration.test-cpu-qemu]==] IN_LIST AXL_IT_COSTED)' <<<"$em")" \
    "if(NOT [==[integration.test-cpu-qemu]==] IN_LIST AXL_IT_COSTED)
    set_tests_properties([==[integration.test-cpu-qemu]==] PROPERTIES COST 7)
endif()"
# The lint holds cpu= to a whole number in range; the emitter alone would
# fall back to 1 and say nothing.
for _c in 0 abc 9 2.5 08 9223372036854775808; do
    printf '#!/bin/bash\n# test-meta: arch=x64 est=1 cpu=%s\n' "$_c" > "$cpud/test-cpu-qemu.sh"
    rc=0; o=$(TEST_LINT_DIR="$cpud" bash lib/discover.sh --lint 2>&1) || rc=$?
    check "cpu=$_c fails the lint, by name" "$rc $(grep -F 'test-cpu-qemu.sh: cpu=' <<<"$o")" \
        "1 test-cpu-qemu.sh: cpu=$_c is not a whole number from 1 to 8"
done
for _c in 1 8; do
    printf '#!/bin/bash\n# test-meta: arch=x64 est=1 cpu=%s\n' "$_c" > "$cpud/test-cpu-qemu.sh"
    rc=0; TEST_LINT_DIR="$cpud" bash lib/discover.sh --lint >/dev/null 2>&1 || rc=$?
    check "control: cpu=$_c passes the lint" "$rc" "0"
done
rm -rf "${cpud:?}"

[[ $fail -eq 0 ]] && echo "discover selftest: OK" || { echo "discover selftest: FAILED"; exit 1; }
