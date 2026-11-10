#!/bin/bash
# test-meta: arch=x64 needs= est=3 local-only=0
# test-test-cache.sh — the skip cache must miss whenever anything it claims to
# cover changes, and must refuse to answer when it cannot see.
#
# Host-only; boots nothing. It drives lib/test-cache.sh against synthetic
# inputs, because the properties that matter are all about WHEN a key misses,
# and a real QEMU run can only demonstrate one of them per boot.
#
# The dangerous direction is a FALSE HIT -- a skip of a test that would have
# failed. So every assertion here except the two controls is "this change must
# BUST the key". The controls exist because a cache that never hits is
# trivially sound and completely useless, and would pass a file made only of
# miss assertions.
#
# Usage: ./test/integration/test-test-cache.sh [--arch X64]

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
cd "$PROJECT_DIR"
export TEST_SKIP_RATCHET=1
# What the runner hands every test must not reach the fixture's key: the
# project its builds= trees are read from (AXL_ONE_PROJECT_DIR -- the REAL
# tree, where fx-goal is unknown), the pooled mode and identity, a BUILD.
unset AXL_ONE_PROJECT_DIR AXL_POOL_CODE_ID AXL_TEST_CACHE_MODE BUILD

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# A private copy of the harness, so mutating "the harness" in this test cannot
# touch the real tree. lib/test-cache.sh resolves its siblings relative to its
# OWN path, so a copied tree is a complete, isolated fixture. It mirrors the
# real layout (test/integration beside scripts/) INSIDE $WORK: the fixture was
# once $WORK/fix, which put its "../../scripts" at /tmp/scripts -- shared by
# every concurrent run and never cleaned up.
FIX="$WORK/test/integration"
FIXS="$WORK/scripts"
mkdir -p "$FIX/lib" "$FIXS"
cp "$SCRIPT_DIR/lib/test-cache.sh" "$SCRIPT_DIR/lib/build-products.sh" "$SCRIPT_DIR/lib/discover.sh" "$FIX/lib/"
printf 'x\n' > "$FIX/common-test.sh"
printf 'x\n' > "$FIXS/run-qemu.sh"
# The build's own name for a tree (lib/build-products.sh asks it): a stub
# that answers as scripts/axl-build.sh --print-prefix --abs does.
cat > "$FIXS/axl-build.sh" <<'STUB'
#!/bin/bash
root="$(cd "$(dirname "$0")/.." && pwd)" arch=x64 cfg=debug
# ...and, as the real one, BUILD from the environment when no argument says.
[[ "${BUILD:-}" == RELEASE ]] && cfg=release
for a in "$@"; do
    case "$a" in
        ARCH=X64|ARCH=x64) arch=x64 ;; ARCH=AARCH64|ARCH=aa64) arch=aa64 ;;
        BUILD=RELEASE) cfg=release ;; BUILD=DEBUG) cfg=debug ;;
    esac
done
printf '%s/build/%s-%s\n' "$root" "$arch" "$cfg"
STUB
chmod +x "$FIXS/axl-build.sh"
# The fixture test NAMES another test, as test-net-latency-starved-qemu.sh
# names (and runs) test-net-latency-qemu.sh; it declares a builds= target
# and names tree files literally, as real tests do.
FIXTURE_BODY='# test-meta: arch=x64 builds=fx-goal
x; runs test-named.sh
cp "$PROJECT_DIR/share/data.json5" "$TEST_STAGING/"
OTHER="${PROJECT_DIR}/share/other.json5"
cat "$PROJECT_DIR/build/x64-debug/run.log"
cp -r "$PROJECT_DIR/share/dir" "$TEST_STAGING/"'
printf '%s\n' "$FIXTURE_BODY" > "$FIX/test-fixture.sh"
mkdir -p "$WORK/share/dir" "$WORK/build/x64-debug" "$WORK/build/x64-release" "$WORK/build/aa64-debug"
for f in share/data.json5 share/other.json5 share/dir/in-dir.json5 build/x64-debug/run.log; do
    printf 'x\n' > "$WORK/$f"
done
# A build graph: fx-goal's product is fx.efi (an object beside it is not a
# product, nor is a utility target's symbolic output).
fx_graph() {   # fx_graph TREE PRODUCT
    cat > "$1/build.ninja" <<NINJA
rule cp
  command = cp \$in \$out
build $2: cp src.c
build fx.o: cp src.c
build CMakeFiles/fx-util: cp src.c
build fx-inner.efi: cp src.c
build fx-goal: phony $2 fx.o CMakeFiles/fx-util || fx-inner.efi
NINJA
    printf 'x\n' > "$1/$2"; printf 'x\n' > "$1/fx.o"; printf 'x\n' > "$1/fx-inner.efi"
}
fx_graph "$WORK/build/x64-debug" fx.efi
fx_graph "$WORK/build/x64-release" fx-rel.efi
fx_graph "$WORK/build/aa64-debug" fx.efi
# One file of each kind the harness reaches without recording it: sourced
# (axl-common.sh, a .conf), executed (sdk-prefix.sh), a scripts/ subdirectory,
# a test/integration helper, lib/ subdirectory, data and fixture, the pool's
# code and its test cases (named guest_cases.py: the real *-cases.py form
# is what discover.sh reads as a guest launch), and a test the fixture names -- and the files that
# must NOT key: another test's own script, a dotfile the runner rewrites every
# run, and Python's bytecode cache.
HARNESS=(scripts/axl-common.sh scripts/axl-toolchains.conf scripts/sdk-prefix.sh
         scripts/console-visual/launch.py
         test/integration/release-fixture.sh test/integration/peer.py
         test/integration/lib/sub/helper.sh test/integration/data/table.json
         test/integration/fixtures/blob.bin
         test/pool/axlpool/service.py test/pool/guest_cases.py test/pool/classes.toml
         test/integration/test-named.sh)
for f in "${HARNESS[@]}"; do mkdir -p "$WORK/${f%/*}"; printf 'x\n' > "$WORK/$f"; done
printf 'x\n' > "$FIX/test-other.sh"
printf 'x\n' > "$FIX/.last-run-stamp"
mkdir -p "$FIXS/__pycache__"; printf 'x\n' > "$FIXS/__pycache__/axl.cpython-312.pyc"

export AXL_TEST_CACHE="$WORK/cache"; mkdir -p "$AXL_TEST_CACHE"
export ARCH=X64
# shellcheck source=lib/test-cache.sh
source "$FIX/lib/test-cache.sh"

ART="$WORK/artifact.efi"; printf 'v1\n' > "$ART"
T=test-fixture.sh

prime() {           # a green run: record inputs, commit the key
    cache_begin_inputs "$T"
    cache_record_input "$T" "$ART"
    cache_commit "$T"
}

echo "=== the skip cache misses on every input it claims to cover ==="

# --- control 1: it can hit at all -------------------------------------------
prime
if cache_is_fresh "$T"; then pass "an unchanged input set is FRESH (the cache can hit)"
else fail "an unchanged input set was not fresh — every miss assertion below is vacuous"; fi

# --- a changed ARTIFACT ------------------------------------------------------
printf 'v2\n' > "$ART"
if cache_is_fresh "$T"; then fail "a changed artifact still read FRESH — this is the false hit that matters"
else pass "a changed artifact busts the key"; fi
printf 'v1\n' > "$ART"; prime

# --- a changed TEST SCRIPT ---------------------------------------------------
printf 'y\n' > "$FIX/test-fixture.sh"
if cache_is_fresh "$T"; then fail "a changed test script still read FRESH"
else pass "a changed test script busts the key"; fi
printf '%s\n' "$FIXTURE_BODY" > "$FIX/test-fixture.sh"; prime

# --- a changed HARNESS -------------------------------------------------------
printf 'y\n' > "$FIX/common-test.sh"
if cache_is_fresh "$T"; then fail "a changed common-test.sh still read FRESH"
else pass "a changed harness busts the key"; fi
printf 'x\n' > "$FIX/common-test.sh"; prime

# --- a changed run-qemu.sh ---------------------------------------------------
printf 'y\n' > "$FIXS/run-qemu.sh"
if cache_is_fresh "$T"; then fail "a changed run-qemu.sh still read FRESH"
else pass "a changed run-qemu.sh busts the key"; fi
printf 'x\n' > "$FIXS/run-qemu.sh"; prime

# --- every kind of harness file -----------------------------------------------
# The key once hashed only common-test.sh, run-qemu.sh and lib/*.sh, so a
# change to scripts/axl-common.sh (which both source) left every cached test
# "green" without running it; likewise a sourced .conf, an executed prefix
# script, a helper the tests run, the pool's code (its own tests run under a
# BOOTED key), and a test this one runs. Each must bust the key.
for f in "${HARNESS[@]}"; do
    printf 'y\n' > "$WORK/$f"
    if cache_is_fresh "$T"; then fail "a changed $f still read FRESH"
    else pass "a changed $f busts the key"; fi
    printf 'x\n' > "$WORK/$f"; prime
done
# ...a harness file that is GONE cannot be proven unchanged...
mv "$FIXS/axl-common.sh" "$WORK/axl-common.sh.gone"
if cache_is_fresh "$T"; then fail "a removed scripts/axl-common.sh still read FRESH"
else pass "a removed harness file busts the key"; fi
mv "$WORK/axl-common.sh.gone" "$FIXS/axl-common.sh"; prime
# ...and a NEW one changes what the harness is.
printf 'x\n' > "$FIXS/new-helper.sh"
if cache_is_fresh "$T"; then fail "a new scripts/new-helper.sh still read FRESH"
else pass "a new harness file busts the key"; fi
rm -f "$FIXS/new-helper.sh"; prime

# --- a test named by a test this one names ------------------------------------
# The chain is followed to its end, not one level: a test that runs a test
# that runs a third is keyed on all three.
printf 'x; runs test-deep.sh\n' > "$FIX/test-named.sh"; printf 'x\n' > "$FIX/test-deep.sh"; prime
printf 'y\n' > "$FIX/test-deep.sh"
if cache_is_fresh "$T"; then fail "a changed test-deep.sh (named by test-named.sh, named by this test) still read FRESH"
else pass "a test two levels down the chain busts the key"; fi
printf 'x\n' > "$FIX/test-named.sh"; rm -f "$FIX/test-deep.sh"; prime

# --- tree files the script NAMES are keyed without being recorded --------------
# A test that copies a tree file into the guest without test_record_input was
# outside its key: edit the file and the test read green. Every literal
# $PROJECT_DIR/... path naming an existing file is keyed now -- in the test
# and in every test it names -- however it is used (a cp, or a variable set
# from it).
bust() {   # bust FILE WHAT -- FILE changed must miss the key
    printf 'y\n' > "$1"
    if cache_is_fresh "$T"; then fail "$2 still read FRESH"
    else pass "$2 busts the key"; fi
    printf 'x\n' > "$1"; prime
}
keep() {   # keep FILE WHAT -- FILE changed must NOT miss the key
    printf 'y\n' > "$1"
    if cache_is_fresh "$T"; then pass "$2 does not bust the key"
    else fail "$2 busted the key"; fi
    printf 'x\n' > "$1"; prime
}
prime
bust "$WORK/share/data.json5" 'a tree file the test copies by a literal path'
bust "$WORK/share/other.json5" 'a tree file a variable is set from (${PROJECT_DIR} spelling)'
printf 'x\ncp "$PROJECT_DIR/share/named.json5" x\n' > "$FIX/test-named.sh"
printf 'x\n' > "$WORK/share/named.json5"; prime
bust "$WORK/share/named.json5" 'a tree file a NAMED test copies by a literal path'
printf 'x\n' > "$FIX/test-named.sh"; rm -f "$WORK/share/named.json5"; prime
# Decided, not missed: a DIRECTORY is not hashed (deps/json-corpora holds
# thousands of files; scripts/check-test-inputs.py makes a copy or mount of
# one say why), and nothing under the build trees (build/, out/, stage/) is a
# source: what the build makes is keyed by builds= (below) or recorded, and a
# run's own logs there would miss every key.
keep "$WORK/share/dir/in-dir.json5" 'a file inside a literally named directory'
keep "$WORK/build/x64-debug/run.log" 'a literal path under build/'

# --- what the test's builds= targets PRODUCE is keyed ---------------------------
# Read from the tree's own build graph (lib/build-products.sh): fx-goal's
# product is fx.efi. The object beside it is an intermediate, not a product.
bust "$WORK/build/x64-debug/fx.efi" "a builds= target's product"
keep "$WORK/build/x64-debug/fx.o" 'an object the goal also reaches'
# ...and an ORDER-ONLY input is a product too: CMake hangs a utility target's
# dependencies on `||`, and a service's inner image, built so, is a file
# tests stage (stderr_tick-dxe.efi).
bust "$WORK/build/x64-debug/fx-inner.efi" "a builds= target's ORDER-ONLY product"
# The graph is memoised; a regenerated build.ninja must be re-read, never
# answered from the old memo.
fx_graph "$WORK/build/x64-debug" fx2.efi; prime
bust "$WORK/build/x64-debug/fx2.efi" 'a product of a REGENERATED graph (the memo is not stale)'
fx_graph "$WORK/build/x64-debug" fx.efi; rm -f "$WORK/build/x64-debug/fx2.efi"; prime
# builds= variables pick the tree, as lib/suite-prep.sh builds it.
printf '%s\n' "${FIXTURE_BODY/builds=fx-goal/builds=BUILD=RELEASE,fx-goal}" > "$FIX/test-fixture.sh"; prime
bust "$WORK/build/x64-release/fx-rel.efi" 'a product in the tree BUILD=RELEASE names'
printf '%s\n' "$FIXTURE_BODY" > "$FIX/test-fixture.sh"; prime
# ...and with no BUILD= in builds=, the tree the ENVIRONMENT names, as the
# runner's pre-build and the test's own build-prefix.sh resolve it: forcing
# DEBUG keyed the debug tree while a BUILD=RELEASE run staged the release one.
export BUILD=RELEASE; prime
bust "$WORK/build/x64-release/fx-rel.efi" 'a product of the tree an exported BUILD=RELEASE names'
unset BUILD; prime
# NOT a named test's builds= products: a name is mostly a mention in a
# comment, and keying what it builds would miss on unrelated images and leave
# no key when the run's prep did not build them (lib/test-cache.sh).
printf '# test-meta: arch=x64 builds=named-goal\nx\n' > "$FIX/test-named.sh"
printf 'build named.efi: cp src.c\nbuild named-goal: phony named.efi\n' >> "$WORK/build/x64-debug/build.ninja"
printf 'x\n' > "$WORK/build/x64-debug/named.efi"; prime
keep "$WORK/build/x64-debug/named.efi" "a NAMED test's builds= product"
rm -f "$WORK/build/x64-debug/named.efi"
if cache_is_fresh "$T"; then pass "...nor does a named test's UNBUILT product take this test's key away"
else fail "a named test's unbuilt product left this test with no key"; fi
printf 'x\n' > "$FIX/test-named.sh"
# Two targets whose memo names once collided (a/b and a_b, `/` -> `_`): each
# keeps its own products.
printf 'build ab-slash.efi: cp src.c\nbuild ab-under.efi: cp src.c\nbuild a/b: phony ab-slash.efi\nbuild a_b: phony ab-under.efi\n' \
    >> "$WORK/build/x64-debug/build.ninja"
printf 'x\n' > "$WORK/build/x64-debug/ab-slash.efi"; printf 'x\n' > "$WORK/build/x64-debug/ab-under.efi"
printf '# test-meta: arch=x64 builds=a_b\nx\n' > "$FIX/test-other.sh"
cache_begin_inputs test-other.sh; cache_record_input test-other.sh "$ART"; cache_key test-other.sh > /dev/null
printf '%s\n' "${FIXTURE_BODY/builds=fx-goal/builds=fx-goal,a/b}" > "$FIX/test-fixture.sh"; prime
bust "$WORK/build/x64-debug/ab-slash.efi" "target a/b's product, with a_b's read first (no memo collision)"
printf '%s\n' "$FIXTURE_BODY" > "$FIX/test-fixture.sh"; printf 'x\n' > "$FIX/test-other.sh"
fx_graph "$WORK/build/x64-debug" fx.efi; prime

# Each of these is an answer the reader could not give: none may leave a key.
nokey() {   # nokey WHAT -- after a green run in the current state, no hit
    cache_invalidate "$T"; prime
    if cache_is_fresh "$T"; then fail "$1 left a FRESH key (keyed over less)"
    else pass "$1 leaves no key (the test runs)"; fi
}
mv "$WORK/build/x64-debug/fx.efi" "$WORK/fx.efi.gone"
nokey 'a builds= product that does not exist'
mv "$WORK/fx.efi.gone" "$WORK/build/x64-debug/fx.efi"
printf '%s\n' "${FIXTURE_BODY/builds=fx-goal/builds=fx-goal,no-such-goal}" > "$FIX/test-fixture.sh"
nokey 'a builds= target ninja does not know'
printf '%s\n' "${FIXTURE_BODY/builds=fx-goal/builds=FROB=1,fx-goal}" > "$FIX/test-fixture.sh"
nokey 'a builds= variable the reader does not know'
# A target that makes no file the reader can see (a utility command, its
# output symbolic) keys nothing: no answer.
printf '%s\n' "${FIXTURE_BODY/builds=fx-goal/builds=fx-goal,fx-util-only}" > "$FIX/test-fixture.sh"
printf 'build fx-util-only: phony CMakeFiles/fx-util\n' >> "$WORK/build/x64-debug/build.ninja"
nokey 'a builds= target that produces no file'
fx_graph "$WORK/build/x64-debug" fx.efi
printf '%s\n' "$FIXTURE_BODY" > "$FIX/test-fixture.sh"
mv "$WORK/build/x64-debug/build.ninja" "$WORK/build.ninja.gone"
nokey 'an unconfigured tree (no build.ninja)'
mv "$WORK/build.ninja.gone" "$WORK/build/x64-debug/build.ninja"
# A PATH with every tool the key uses except ninja (rm -rf'd memo first, or
# the memo would answer without asking ninja at all).
mkdir -p "$WORK/nobin"
for tool in bash sha256sum cut sort tr xargs sed grep find stat awk mkdir mv rm cat tail \
            dirname basename env; do
    ln -sf "$(command -v "$tool")" "$WORK/nobin/$tool"
done
rm -rf "$AXL_TEST_CACHE/.products"
# shellcheck disable=SC2123 # replacing the search path IS the case
_opath="$PATH"; PATH="$WORK/nobin"; hash -r
nokey 'a ninja that cannot run'
# The control: that PATH plus ninja keys -- so ninja, and nothing else
# missing from it, is what left no key above.
PATH="$_opath" ln -sf "$(PATH="$_opath" command -v ninja)" "$WORK/nobin/ninja"; rm -rf "$AXL_TEST_CACHE/.products"
prime
if cache_is_fresh "$T"; then pass "...the same PATH WITH ninja keys (the control: ninja was the missing tool)"
else fail "the reduced PATH leaves no key even with ninja -- the case above proves nothing"; fi
PATH="$_opath"; hash -r
# A ninja that RUNS but answers nothing (a broken or foreign one) is no
# answer either: a node it said nothing about would read as a source file --
# no products -- and key over less.
mkdir -p "$WORK/mute"; printf '#!/bin/bash\nexit 0\n' > "$WORK/mute/ninja"; chmod +x "$WORK/mute/ninja"
rm -rf "$AXL_TEST_CACHE/.products"
_opath="$PATH"; PATH="$WORK/mute:$PATH"; hash -r
nokey 'a ninja that runs but says nothing about the target'
PATH="$_opath"; hash -r; rm -rf "$AXL_TEST_CACHE/.products"
# The reader itself, run as the gate runs it: each refusal is SAID, with an
# exit 1 -- not left to the hash of a path that happens not to exist.
bpr() { bash "$FIX/lib/build-products.sh" X64 "$FIX/test-fixture.sh" 2>&1; echo "rc=$?"; }
mv "$WORK/build/x64-debug/fx.efi" "$WORK/fx.efi.gone"
if [[ "$(bpr)" == "build-products: $FIX/test-fixture.sh: builds= product $WORK/build/x64-debug/fx.efi does not exist (not built?)"$'\n'"rc=1" ]]; then
    pass "build-products.sh: a product that does not exist is refused, by name (exit 1)"
else fail "build-products.sh, a missing product: [$(bpr)]"; fi
mv "$WORK/fx.efi.gone" "$WORK/build/x64-debug/fx.efi"
printf '%s\n' "${FIXTURE_BODY/builds=fx-goal/builds=FROB=1,fx-goal}" > "$FIX/test-fixture.sh"
if [[ "$(bpr)" == "build-products: builds= variable 'FROB=1' is not one this reader knows"$'\n'"rc=1" ]]; then
    pass "build-products.sh: an unknown builds= variable is refused, by name (exit 1)"
else fail "build-products.sh, an unknown variable: [$(bpr)]"; fi
printf '%s\n' "$FIXTURE_BODY" > "$FIX/test-fixture.sh"
if [[ "$(bpr)" == "$FIX/test-fixture.sh"$'\t'"$WORK/build/x64-debug/fx-inner.efi"$'\n'"$FIX/test-fixture.sh"$'\t'"$WORK/build/x64-debug/fx.efi"$'\n'"rc=0" ]]; then
    pass "build-products.sh: the control -- the products, exactly, and exit 0"
else fail "build-products.sh, the control: [$(bpr)]"; fi
prime
if cache_is_fresh "$T"; then pass "...and with every reader answering again, the cache hits (the cases above were not vacuous)"
else fail "the cache stopped hitting after the builds= cases -- their restores are broken"; fi

# --- a POOLED run's key holds the code the pool service is running ------------
# The guests come from the long-lived service, which may run other code than
# this tree's test/pool/ (an older start; the main checkout's copy). The
# runner asks the service once per run (AXL_POOL_CODE_ID); a pooled key
# includes it, and with no answer there is no pooled key at all.
export AXL_TEST_CACHE_MODE=pool
unset AXL_POOL_CODE_ID
nokey 'a pooled run with no code identity from the service'
export AXL_POOL_CODE_ID=aaaa; prime
if cache_is_fresh "$T"; then pass "a pooled run under an unchanged service code identity is FRESH"
else fail "a pooled run under the same service code identity missed -- the pooled cache never hits"; fi
export AXL_POOL_CODE_ID=bbbb
if cache_is_fresh "$T"; then fail "a pooled key matched under ANOTHER service code identity"
else pass "a changed service code identity busts the pooled key"; fi
unset AXL_POOL_CODE_ID
export AXL_TEST_CACHE_MODE=
prime

# --- an ERROR is not an empty harness ------------------------------------------
# A find or a read that fails yields a SHORTER list, and a key over less is
# stable from one run to the next: the error present at the green run and
# still present at the next gives a matching key -- a false hit. So each case
# breaks the harness BEFORE the green run: with an error, neither may leave a
# key to match.
if [[ $EUID -eq 0 ]]; then
    # root reads a mode-000 file and directory, so these cannot be staged.
    for c in "a find error" "...without pipefail" "an unreadable harness file" "...and readable again"; do
        echo "  SKIP: $c leaves no key (running as root: chmod 000 does not stop root)"
    done
else
    mkdir -p "$FIXS/locked"; printf 'x\n' > "$FIXS/locked/f.sh"
    cache_invalidate "$T"; chmod 000 "$FIXS/locked"; prime
    if cache_is_fresh "$T"; then fail "a scripts/ directory find could not read left a FRESH key (keyed over the shorter list)"
    else pass "a find error leaves no key (the test runs)"; fi
    # ...whatever the CALLER's shell options: this file runs with pipefail, and a
    # caller without it must not lose the find's status in the key's pipeline.
    ( set +o pipefail
      cache_invalidate "$T"; prime
      if cache_is_fresh "$T"; then exit 1; else exit 0; fi )
    if [[ $? -eq 0 ]]; then pass "a find error leaves no key in a caller without pipefail too"
    else fail "without pipefail in the caller, a find error left a FRESH key"; fi
    chmod 755 "$FIXS/locked"; rm -rf "$FIXS/locked"
    cache_invalidate "$T"; chmod 000 "$FIXS/axl-toolchains.conf"; prime
    if cache_is_fresh "$T"; then fail "an unreadable scripts/axl-toolchains.conf left a FRESH key (keyed without it)"
    else pass "an unreadable harness file leaves no key (the test runs)"; fi
    chmod 644 "$FIXS/axl-toolchains.conf"; prime
    if cache_is_fresh "$T"; then pass "...and once readable again, the cache hits (the error cases were not vacuous)"
    else fail "the cache stopped hitting after the error cases -- their restores are broken"; fi
fi

# --- precision: what must NOT bust it -----------------------------------------
# A cache that misses on everything is sound and useless; these keep it useful.
printf 'y\n' > "$FIX/test-other.sh"
if cache_is_fresh "$T"; then pass "a test this one does not name does not bust its key"
else fail "editing test-other.sh busted test-fixture.sh's key -- every edit to any test would re-run all"; fi
printf 'x\n' > "$FIX/test-other.sh"; prime
printf 'y\n' > "$FIX/.last-run-stamp"
if cache_is_fresh "$T"; then pass "a dotfile the runner rewrites (.last-run-stamp) does not bust the key"
else fail ".last-run-stamp busted the key -- every run would miss every key"; fi
printf 'x\n' > "$FIX/.last-run-stamp"; prime
printf 'y\n' > "$FIXS/__pycache__/axl.cpython-312.pyc"
if cache_is_fresh "$T"; then pass "a rewritten __pycache__ .pyc does not bust the key"
else fail "a .pyc busted the key -- any Python helper's run would miss every key"; fi
printf 'x\n' > "$FIXS/__pycache__/axl.cpython-312.pyc"; prime
printf 'y\n' > "$FIXS/.stamp"
if cache_is_fresh "$T"; then pass "a dotfile under scripts/ does not bust the key"
else fail "a dotfile under scripts/ busted the key"; fi
rm -f "$FIXS/.stamp"; prime

# --- a changed lib/ ----------------------------------------------------------
printf '# tweak\n' >> "$FIX/lib/test-cache.sh"
if cache_is_fresh "$T"; then fail "a changed lib/*.sh still read FRESH"
else pass "a changed lib/ script busts the key"; fi
cp "$SCRIPT_DIR/lib/test-cache.sh" "$FIX/lib/"; prime

# --- a different ARCH --------------------------------------------------------
ARCH=AARCH64
if cache_is_fresh "$T"; then fail "a different ARCH still read FRESH — the arches would share a key"
else pass "a different arch busts the key"; fi
ARCH=X64

# --- both arches' keys COEXIST ------------------------------------------------
# The key FILE was named by test only, so the two CTest trees (or an X64 then
# an AARCH64 runner run) overwrote each other's key and every run of either
# missed. The arch is in the file name now. AXL_TEST_CACHE_ARCH is how the
# runner tells the TEST process (which records inputs) the arch: both sides
# must name the same file.
prime
export AXL_TEST_CACHE_ARCH=AARCH64; ARCH=AARCH64; prime
export AXL_TEST_CACHE_ARCH=X64; ARCH=X64
if cache_is_fresh "$T"; then pass "an AARCH64 green run leaves the X64 key standing (per-arch key files)"
else fail "an AARCH64 green run overwrote the X64 key -- the two trees would thrash"; fi
export AXL_TEST_CACHE_ARCH=AARCH64; ARCH=AARCH64
if cache_is_fresh "$T"; then pass "...and the AARCH64 key stands too"
else fail "the AARCH64 key did not survive"; fi
export AXL_TEST_CACHE_ARCH=X64; ARCH=X64
# run-qemu.sh records too, through the same cache_record_input -- in a
# process whose own ARCH may be anything (its variables, not the runner's).
# AXL_TEST_CACHE_ARCH, which the runner exports, must win, or it would record
# into a file the runner never reads and the key would silently lose inputs.
ARCH=bogus-run-qemu-value
cache_record_input "$T" "$ART"
if [[ -s "$AXL_TEST_CACHE/$T@X64.inputs" && ! -e "$AXL_TEST_CACHE/$T@bogus-run-qemu-value.inputs" ]]; then
    pass "a recorder with another ARCH still records into the runner's file (AXL_TEST_CACHE_ARCH wins)"
else fail "a recorder's own ARCH redirected the recording"; fi
ARCH=X64

# --- a stray AXL_TLS must NOT bust the key ------------------------------------
# INVERTED. AXL_TLS used to select which sources compiled, so it belonged in
# the key. mbedTLS is unconditional now: keying on it would mean a leftover
# variable in someone's environment silently re-ran the whole suite from cold
# for no reason at all.
AXL_TLS=0
if cache_is_fresh "$T"; then pass "a stray AXL_TLS does not bust the key"
else fail "a stray AXL_TLS still busts the cache key"; fi
unset AXL_TLS

# --- a VANISHED input --------------------------------------------------------
# "cannot prove unchanged" must read as a miss, never as a hit.
mv "$ART" "$ART.gone"
if cache_is_fresh "$T"; then fail "a vanished input still read FRESH — the cache answered what it cannot see"
else pass "a vanished input busts the key"; fi
mv "$ART.gone" "$ART"; prime

# --- no record at all --------------------------------------------------------
if cache_is_fresh "test-never-run.sh"; then fail "a test with NO record read FRESH"
else pass "a test with no record is never fresh"; fi

# --- a RED run must not leave a usable key -----------------------------------
cache_invalidate "$T"
if cache_is_fresh "$T"; then fail "a failing run left a usable key — a red test could be skipped"
else pass "cache_invalidate drops the key (a red test is never skipped)"; fi
prime

# --- control 2: still hits after all that ------------------------------------
if cache_is_fresh "$T"; then pass "the cache still hits after every restore (miss assertions were real)"
else fail "the cache stopped hitting — the restores above are broken, not the cache"; fi

# --- off by default ----------------------------------------------------------
( unset AXL_TEST_CACHE
  if cache_is_fresh "$T"; then exit 1; else exit 0; fi )
if [[ $? -eq 0 ]]; then pass "with AXL_TEST_CACHE unset the cache never claims freshness"
else fail "the cache answered with AXL_TEST_CACHE unset — it is not opt-in"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]]
