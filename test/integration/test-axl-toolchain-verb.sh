#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-axl-toolchain-verb.sh — `axl toolchain list` / `install` / `uninstall`.
#
# WHY THIS EXISTS. The dispatcher managed SDK versions and toolchain versions
# with completely different vocabularies: `axl list` / `axl use` for the SDK,
# and for the toolchain a separate program name (`axl-install-toolchain`) that
# you had to already know. There was no way to ask the question every consumer
# actually has -- "do I have the compiler this SDK pins?" -- short of reading
# axl-toolchains.conf and stat-ing a path out of it by hand.
#
# NO `update` VERB, deliberately. The toolchain is not user-versioned: the SDK
# prefix pins it, so moving between toolchains IS `axl use <sdk version>`. An
# `axl toolchain update` would be a second, silently divergent way to change
# what `axl-cc` compiles with.
#
# THERE ARE THREE OPERATIONS: `list`, `install` and `uninstall`. This said "the
# operations are install and list" until x64 defaulted to AXL_TOOLCHAIN=auto,
# which made the toolchain's PRESENCE the thing that selects the compiler
# (AXL-Host-Toolchain-Design.md §6.2): removing it stopped being a tidy-up and
# became how a user chooses host gcc, so it had to become a managed operation
# carrying the §21 ownership guard rather than an `rm -rf` typed by hand at a
# path read out of a manifest. `update` is still absent, for the reason above
# -- a third operation is not a fourth.
#
# EVERY `uninstall` case in THIS file is a REFUSAL, and case 17 says why: the
# file runs on the developer's own box against the real /opt toolchain, which
# carries a valid receipt and would therefore be removed rather than refused.
# The removal round trip lives in test-install-lifecycle.sh, inside podman,
# against a root that test creates in the container.
#
# THE RESOLUTION IS THE INTERESTING PART. Since §20 M2 `axl` is linked out of
# the MANAGER root, which holds no manifest and no axl-install-toolchain --
# both are SDK content. A verb that read them from $PREFIX would work in a
# source checkout and fail on every real install, which is exactly how
# `axl prune` came to prune nothing. So both verbs resolve the CURRENT SDK,
# the same way --print-prefix does, and the cases below run from a manager
# root to prove it.
#
# Host-only: synthetic prefixes under a temp dir, no network. Case 7 is the
# one exception worth naming -- it copies the REAL manifest to prove the
# source-checkout layout is read, so it probes whatever /opt toolchain the
# machine happens to have. Read-only, and it asserts nothing about /opt.
#
# Usage: ./test/integration/test-axl-toolchain-verb.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

WORK="$(mktemp -d -t axl-tcverb.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Isolate ONE arch's block from a `toolchain list` transcript. The loop always
# prints aa64 first and x64 second, so aa64's block runs up to the `x64`
# line. Scoping matters for the "builds with" assertions below: an UNSCOPED
# `grep -q 'builds with: axl'` also matches aa64's fixed "(aa64 has no host
# option)" line, so it can pass even when x64 itself is broken. Caught the
# hard way -- a deliberate axl/host swap sabotage of x64's resolution still
# passed an unscoped assertion for the wrong reason.
#
# x64's block STOPS at the blank line before the trailing "install with: ..."
# footer -- NOT run to EOF. x64 is the last arch printed, so running to EOF
# used to also swallow that footer, and when it names x64 by itself ("axl
# toolchain install x64", case 8's shape) an assertion for that exact string
# became satisfiable by the footer instead of by the real per-arch text it
# was meant to check -- free whenever a fixture leaves x64 individually
# MISSING while its "builds with" line says something other than host.
x64_block()  { awk '/^  x64/{f=1} f && /^$/{exit} f' "$1"; }
aa64_block() { awk '/^  aa64/{f=1} /^  x64/{f=0} f' "$1"; }

# A realistic install: an SDK prefix carrying the manifest and the installer,
# a manager root carrying `axl` and NEITHER, and both `current` markers.
# $1 = "installed" to also create the toolchain directories.
setup() {
    local want_tc="${1-}"
    rm -rf "$WORK/opt"; mkdir -p "$WORK/opt"
    local sdk="$WORK/opt/axl-sdk-4.6.0"
    mkdir -p "$sdk/bin" "$sdk/libexec/axl" "$sdk/share/axl"
    echo "4.6.0" > "$sdk/share/axl/version"
    cat > "$sdk/share/axl/axl-toolchains.conf" <<EOF
AXL_AA64_TOOLCHAIN_VERSION=14.3.rel1
AXL_AA64_TOOLCHAIN_DIR=$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
AXL_AA64_GXX_DEFAULT=$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-g++
AXL_AA64_GCC_DEFAULT=$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-gcc
AXL_X64_TOOLCHAIN_VERSION=14.3.0-axl3
AXL_X64_TOOLCHAIN_DIR=$WORK/opt/x86_64-elf-gcc-14.3.0-axl3
AXL_X64_GXX_DEFAULT=$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-g++
AXL_X64_GCC_DEFAULT=$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-gcc
EOF
    # A stub installer, so `install` can be observed without a download.
    cat > "$sdk/bin/axl-install-toolchain" <<'EOF'
#!/bin/sh
echo "STUB-INSTALLER args: $*"
echo "STUB-INSTALLER cwd-independent"
EOF
    chmod +x "$sdk/bin/axl-install-toolchain"

    # The REAL axl-cc, not a stub: `axl toolchain list` calls its
    # --print-toolchain (Task 3) rather than re-deriving the resolution, so
    # the case that proves that wiring needs the genuine resolution logic,
    # not a canned answer. It reads the manifest just written above via its
    # own SDK_DIR/share/axl/axl-toolchains.conf lookup -- no other wiring
    # needed for --print-toolchain, which exits before touching headers/libs.
    cp "$PROJECT_DIR/scripts/axl-cc" "$sdk/bin/axl-cc"
    chmod +x "$sdk/bin/axl-cc"

    local mgr="$WORK/opt/axl-sdk-host-tools-4.6.0"
    mkdir -p "$mgr/bin" "$mgr/libexec/axl" "$mgr/share/axl"
    echo "4.6.0" > "$mgr/share/axl/version"
    cp "$PROJECT_DIR/scripts/axl" "$mgr/bin/axl"
    chmod +x "$mgr/bin/axl"
    # The manager deliberately gets NO manifest and NO installer: that is what
    # make-host-tools-tarball.sh produces, and reading them from $PREFIX is the
    # bug these cases exist to prevent.

    ln -sfn "$sdk" "$WORK/opt/axl-sdk"
    ln -sfn "$mgr" "$WORK/opt/axl-sdk-host-tools"

    if [[ "$want_tc" == "installed" ]]; then
        local d
        for d in "$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf" \
                 "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3"; do
            mkdir -p "$d/bin"
        done
        printf '#!/bin/sh\necho "aarch64-none-elf-g++ (Arm GNU Toolchain 14.3.rel1) 14.3.1"\n' \
            > "$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-g++"
        printf '#!/bin/sh\necho "x86_64-elf-g++ (GCC) 14.3.0"\n' \
            > "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-g++"
        # The C compiler stubs, SEPARATE from g++ above: axl-cc's `auto`
        # resolution for --print-toolchain probes the GCC locator, not GXX
        # (a build can be pure C), so a fixture that stands in for "the
        # bare-metal toolchain is installed" needs both or the new "builds
        # with" assertions below would silently probe the wrong binary.
        printf '#!/bin/sh\necho "aarch64-none-elf-gcc (Arm GNU Toolchain 14.3.rel1) 14.3.1"\n' \
            > "$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-gcc"
        printf '#!/bin/sh\necho "x86_64-elf-gcc (GCC) 14.3.0"\n' \
            > "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-gcc"
        chmod +x "$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-g++" \
                 "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-g++" \
                 "$WORK/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-gcc" \
                 "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-gcc"
    fi
}

AXL() { "$WORK/opt/axl-sdk-host-tools-4.6.0/bin/axl" "$@"; }

echo "=== 1. list: reports what is MISSING, from a manager root ==="
setup
OUT="$WORK/list-missing.txt"
AXL toolchain list > "$OUT" 2>&1
rc=$?
if [[ $rc -eq 0 ]]; then
    pass "list exits 0"
else
    fail "list exits 0 (got $rc)"
    sed 's/^/      /' "$OUT" | head -8
fi
# Both arches must appear with their PINNED versions -- read out of the SDK's
# manifest, which the prefix `axl` runs from does not have.
if grep -q 'aa64' "$OUT" && grep -q '14.3.rel1' "$OUT"; then
    pass "names aa64 and its pinned version"
else
    fail "aa64 / 14.3.rel1 missing from the listing"
    sed 's/^/      /' "$OUT" | head -8
fi
if grep -q 'x64' "$OUT" && grep -q '14.3.0-axl3' "$OUT"; then
    pass "names x64 and its pinned version"
else
    fail "x64 / 14.3.0-axl3 missing from the listing"
    sed 's/^/      /' "$OUT" | head -8
fi
if [[ "$(grep -c 'MISSING' "$OUT")" -eq 2 ]]; then
    pass "both absent toolchains are marked MISSING"
else
    fail "expected exactly 2 MISSING lines, got $(grep -c 'MISSING' "$OUT")"
    sed 's/^/      /' "$OUT" | head -8
fi
# EXACT, because the wrong hint is a working-looking command that does half
# the job: install-toolchain.sh parses one target and takes the last, so
# `install aa64 x64` installs x64 and silently skips aa64. `grep -q "axl
# toolchain install"` passed against that.
if grep -qF 'axl toolchain install all' "$OUT"; then
    pass "with both missing, the hint is 'install all' (not 'aa64 x64')"
else
    fail "hint is not 'axl toolchain install all'"
    grep -F 'install with' "$OUT" | sed 's/^/      /'
fi

echo
echo "=== 2. list: reports what IS installed, with the real compiler ==="
setup installed
OUT2="$WORK/list-ok.txt"
AXL toolchain list > "$OUT2" 2>&1
if ! grep -q 'MISSING' "$OUT2"; then
    pass "nothing is reported MISSING when both are present"
else
    fail "a present toolchain was reported MISSING"
    sed 's/^/      /' "$OUT2" | head -8
fi
# Probing the compiler is the difference between "the directory exists" and
# "the toolchain works" -- a half-extracted tarball leaves the first true.
if grep -q '14.3.1' "$OUT2"; then
    pass "reports the version the compiler itself prints"
else
    fail "never ran the compiler -- a directory check is not an install check"
    sed 's/^/      /' "$OUT2" | head -8
fi

echo
echo "=== 3. install: delegates to the SDK's installer, not the manager's ==="
setup
OUT3="$WORK/install.txt"
AXL toolchain install x64 > "$OUT3" 2>&1
if grep -q 'STUB-INSTALLER args: x64' "$OUT3"; then
    pass "install x64 reaches the SDK's axl-install-toolchain with its arg"
else
    fail "the installer was not invoked with 'x64'"
    sed 's/^/      /' "$OUT3" | head -8
fi
OUT4="$WORK/install-bare.txt"
AXL toolchain install > "$OUT4" 2>&1
if grep -q 'STUB-INSTALLER args:' "$OUT4"; then
    pass "a bare 'install' delegates too (the installer owns the default)"
else
    fail "bare install did not reach the installer"
    sed 's/^/      /' "$OUT4" | head -8
fi

echo
echo "=== 4. the failure modes say which file is missing ==="
setup
rm -f "$WORK/opt/axl-sdk-4.6.0/share/axl/axl-toolchains.conf"
OUT5="$WORK/noconf.txt"
if AXL toolchain list > "$OUT5" 2>&1; then
    fail "list succeeded with no manifest at all"
    sed 's/^/      /' "$OUT5" | head -6
else
    pass "no manifest is an error, not an empty listing"
fi
if grep -q 'axl-toolchains.conf' "$OUT5"; then
    pass "and it names the file it could not read"
else
    fail "the error never names axl-toolchains.conf"
    sed 's/^/      /' "$OUT5" | head -6
fi

setup
rm -f "$WORK/opt/axl-sdk-4.6.0/bin/axl-install-toolchain"
OUT6="$WORK/noinst.txt"
if AXL toolchain install x64 > "$OUT6" 2>&1; then
    fail "install succeeded with no installer present"
else
    pass "a missing axl-install-toolchain is an error"
fi
if grep -q 'axl-install-toolchain' "$OUT6"; then
    pass "and it names what is missing"
else
    fail "the error never names axl-install-toolchain"
    sed 's/^/      /' "$OUT6" | head -6
fi

echo
echo "=== 5. an unknown subcommand is refused, not silently ignored ==="
setup
OUT7="$WORK/bogus.txt"
if AXL toolchain frobnicate > "$OUT7" 2>&1; then
    fail "'axl toolchain frobnicate' exited 0"
else
    pass "an unknown toolchain subcommand fails"
fi
# `update` specifically: the SDK prefix pins the toolchain, so this verb does
# not exist -- and a user who reaches for it deserves the reason, not just a
# usage dump.
OUT8="$WORK/update.txt"
AXL toolchain update > "$OUT8" 2>&1
if grep -q 'axl use' "$OUT8"; then
    pass "'toolchain update' explains that the SDK version pins the toolchain"
else
    fail "'toolchain update' does not point at 'axl use'"
    sed 's/^/      /' "$OUT8" | head -6
fi

echo
echo "=== 6. it is discoverable ==="
setup
# `grep -q toolchain` passed BEFORE the verb existed, because the help text
# already named `axl-install-toolchain`. Match the VERB, two words.
# CAPTURE, THEN GREP -- never `AXL --help | grep -q` here. Under `set -o
# pipefail` that reports FAILURE on a MATCH: grep -q exits at the first hit,
# the still-writing `axl` takes SIGPIPE, and pipefail promotes its 141 to the
# pipeline's status. Measured: PIPESTATUS=(141 0), a match and a "failure".
# It depends on whether the writer has finished before grep leaves, so it is
# also a flaky-test generator -- the same expression passed when I ran it by
# hand and failed inside this file.
AXL --help > "$WORK/help.txt" 2>&1
if grep -qE '(^|[^-])axl toolchain|^ *toolchain ' "$WORK/help.txt"; then
    pass "axl --help mentions the toolchain verb"
else
    fail "axl --help never mentions the 'toolchain' verb itself"
fi

echo
echo "=== 7. a SOURCE CHECKOUT finds the manifest beside the script ==="
# In the tree the manifest is scripts/axl-toolchains.conf; only an INSTALL has
# share/axl/axl-toolchains.conf. install-toolchain.sh has always tried both,
# and a verb that tried only the installed location would fail for every
# developer running it out of a checkout -- the one place it is easiest to
# reach for.
SRC="$WORK/src"
mkdir -p "$SRC/scripts" "$SRC/bin"
cp "$PROJECT_DIR/scripts/axl" "$SRC/bin/axl"; chmod +x "$SRC/bin/axl"
cp "$PROJECT_DIR/scripts/axl-toolchains.conf" "$SRC/scripts/"
OUT9="$WORK/src-list.txt"
if "$SRC/bin/axl" toolchain list > "$OUT9" 2>&1; then
    pass "list works from a source checkout"
else
    fail "list failed in a source checkout"
    sed 's/^/      /' "$OUT9" | head -6
fi
if grep -q 'aa64' "$OUT9" && grep -q 'x64' "$OUT9"; then
    pass "and reads the in-tree scripts/axl-toolchains.conf"
else
    fail "the in-tree manifest was not read"
    sed 's/^/      /' "$OUT9" | head -6
fi

echo "=== 8. one missing arch names THAT arch, not 'all' ==="
setup
mkdir -p "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin"
printf '#!/bin/sh\necho "x86_64-elf-g++ (GCC) 14.3.0"\n' \
    > "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-g++"
chmod +x "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-g++"
OUT10="$WORK/one-missing.txt"
AXL toolchain list > "$OUT10" 2>&1
if grep -qF 'axl toolchain install aa64' "$OUT10"; then
    pass "one missing arch is named on its own"
else
    fail "expected 'axl toolchain install aa64'"
    grep -F 'install with' "$OUT10" | sed 's/^/      /'
fi
if ! grep -qF 'install all' "$OUT10"; then
    pass "and it does not say 'all' when only one is missing"
else
    fail "said 'all' with one arch already installed"
fi

echo
echo "=== 9. AXL_<ARCH>_GXX overrides are honoured ==="
# axl-toolchains.conf documents `${AXL_X64_GXX:-$AXL_X64_GXX_DEFAULT}` and
# axl-cc, install.sh and the Makefile all resolve that way. Reading _DEFAULT
# alone reported MISSING for anyone who installed to a user prefix and
# exported what install-toolchain.sh's own print_env told them to.
setup
ALT="$WORK/alt-toolchain"
mkdir -p "$ALT/bin"
printf '#!/bin/sh\necho "x86_64-elf-g++ (ALT BUILD) 14.3.0"\n' > "$ALT/bin/x86_64-elf-g++"
chmod +x "$ALT/bin/x86_64-elf-g++"
OUT11="$WORK/override.txt"
AXL_X64_GXX="$ALT/bin/x86_64-elf-g++" AXL toolchain list > "$OUT11" 2>&1
if grep -q 'ALT BUILD' "$OUT11"; then
    pass "an overridden compiler is probed and reported"
else
    fail "AXL_X64_GXX was ignored"
    sed 's/^/      /' "$OUT11" | head -8
fi
if grep -qF "$ALT" "$OUT11"; then
    pass "and the toolchain root shown is the overridden one"
else
    fail "still showed the manifest's directory for an overridden compiler"
    sed 's/^/      /' "$OUT11" | head -8
fi
# The control: without the override the same tree reports MISSING, so the
# assertions above cannot be passing on something else.
OUT12="$WORK/no-override.txt"
AXL toolchain list > "$OUT12" 2>&1
if grep -q 'MISSING' "$OUT12" && ! grep -q 'ALT BUILD' "$OUT12"; then
    pass "control: without the override the same tree reports MISSING"
else
    fail "control failed -- case 9 proves nothing"
fi

echo
echo "=== 10. 'list' rejects stray arguments ==="
setup
if AXL toolchain list x64 > "$WORK/stray.txt" 2>&1; then
    fail "'toolchain list x64' silently listed everything"
else
    pass "a stray argument to 'list' is refused"
fi

echo
echo "=== 11. list: names WHICH compiler x64 actually builds with ==="
setup installed
OUT13="$WORK/builds-with.txt"
AXL toolchain list > "$OUT13" 2>&1
if x64_block "$OUT13" | grep -q 'builds with: axl'; then
    pass "list: names the variant x64 will actually build with"
else
    fail "no 'builds with: axl' line for an installed toolchain"
    sed 's/^/      /' "$OUT13" | head -10
fi
if x64_block "$OUT13" | grep -qF 'auto -> axl'; then
    pass "and names the reason: auto -> axl"
else
    fail "the axl resolution did not name its reason"
    sed 's/^/      /' "$OUT13" | head -10
fi

echo
echo "=== 12. list: x64 falls back to host, and says why + the fix ==="
setup installed
OUT14="$WORK/host-fallback.txt"
# Point the C locator at a path that does not exist -- never touch the real
# toolchain under /opt, this is a synthetic tree already.
AXL_X64_GCC=/nonexistent/x86_64-elf-gcc AXL toolchain list > "$OUT14" 2>&1
if x64_block "$OUT14" | grep -q 'builds with: host'; then
    pass "list: reports host when the bare-metal C compiler is absent"
else
    fail "expected 'builds with: host' when AXL_X64_GCC pointed nowhere"
    sed 's/^/      /' "$OUT14" | head -10
fi
if x64_block "$OUT14" | grep -qF 'auto -> host: no bare-metal toolchain installed'; then
    pass "and names why: auto -> host: no bare-metal toolchain installed"
else
    fail "the host fallback did not name its reason"
    sed 's/^/      /' "$OUT14" | head -10
fi
if x64_block "$OUT14" | grep -qF 'C++' && x64_block "$OUT14" | grep -q 'needs the bare-metal toolchain'; then
    pass "and says C++ is unavailable under host"
else
    fail "no note that C++ is unavailable under host"
    sed 's/^/      /' "$OUT14" | head -10
fi
if x64_block "$OUT14" | grep -qF 'axl toolchain install x64'; then
    pass "and names the verb that fixes it"
else
    fail "the C++ note never names 'axl toolchain install x64'"
    sed 's/^/      /' "$OUT14" | head -10
fi
# Control: WITHOUT the override, the very same installed tree reports axl --
# proving the four assertions above are not passing on fixed text.
OUT15="$WORK/host-fallback-control.txt"
AXL toolchain list > "$OUT15" 2>&1
if x64_block "$OUT15" | grep -q 'builds with: axl' && ! x64_block "$OUT15" | grep -q 'builds with: host'; then
    pass "control: without the override, the installed tree says axl"
else
    fail "control failed -- case 12 proves nothing"
    sed 's/^/      /' "$OUT15" | head -10
fi

echo
echo "=== 13. list: aa64 always builds with axl -- no host option exists ==="
setup   # nothing installed at all
OUT16="$WORK/aa64-always-axl.txt"
AXL toolchain list > "$OUT16" 2>&1
if aa64_block "$OUT16" | grep -qF 'builds with: axl (aa64 has no host option)'; then
    pass "aa64's line states plainly that it has no host option"
else
    fail "aa64's 'builds with' line never says it has no host option"
    sed 's/^/      /' "$OUT16" | head -10
fi

echo
echo "=== 14. list: an explicit AXL_TOOLCHAIN pin overrides auto ==="
setup installed
OUT17="$WORK/pinned.txt"
AXL_TOOLCHAIN=host AXL toolchain list > "$OUT17" 2>&1
if x64_block "$OUT17" | grep -qF 'builds with: host (pinned by $AXL_TOOLCHAIN)'; then
    pass "x64: an explicit AXL_TOOLCHAIN=host is reported as pinned, not auto"
else
    fail "AXL_TOOLCHAIN=host was not reported as a pin for x64"
    sed 's/^/      /' "$OUT17" | head -10
fi
# Whole-file: NEITHER arch's line may claim an 'auto' resolution while the
# pin is active -- x64 is pinned and aa64 cannot honor this pin at all
# (next assertion), so 'auto ->' must not appear anywhere in the transcript.
if ! grep -q 'auto -> host' "$OUT17" && ! grep -q 'auto -> axl' "$OUT17"; then
    pass "and no line claims an 'auto' resolution while the pin is active"
else
    fail "a pinned build was described as auto"
    sed 's/^/      /' "$OUT17" | head -10
fi
# aa64 CANNOT build under AXL_TOOLCHAIN=host (axl-cc refuses it by name,
# §4.2) -- the probe genuinely cannot answer, and that must read as an
# honest 'unknown', never as a confident (and wrong) 'axl'.
if aa64_block "$OUT17" | grep -q 'builds with: unknown'; then
    pass "aa64: an impossible pin is reported as unknown, not a silent axl"
else
    fail "aa64's line did not flag the impossible AXL_TOOLCHAIN=host pin"
    sed 's/^/      /' "$OUT17" | head -10
fi

echo
echo "=== 15. list: a gcc-only machine still builds x64 with axl (the g++ trap) ==="
# THE TRAP THIS FEATURE MUST NOT FALL INTO: the EXISTING installed/MISSING
# line above probes g++ (the loop's \$_g); a C-only 'auto' resolution that
# reused it would report x64 as 'host' on a machine with a perfectly good C
# compiler and no C++ one -- wrong, and it looks right. axl-cc's own
# --print-toolchain probes gcc, so the 'builds with' line must too.
setup
mkdir -p "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin"
printf '#!/bin/sh\necho "x86_64-elf-gcc (GCC) 14.3.0"\n' \
    > "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-gcc"
chmod +x "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3/bin/x86_64-elf-gcc"
# Deliberately NO x86_64-elf-g++ -- gcc is present, g++ is absent. aa64 has
# NOTHING installed either, so its own line is a fixed "axl (no host
# option)" regardless of this fixture -- the assertions below are scoped to
# x64_block specifically, or aa64's constant text would pass them for free.
OUT18="$WORK/gcc-only.txt"
AXL toolchain list > "$OUT18" 2>&1
if x64_block "$OUT18" | grep -q 'builds with: axl'; then
    pass "gcc present + g++ absent: x64 still builds with axl"
else
    fail "reported host (or nothing) for x64 with a working C compiler present"
    sed 's/^/      /' "$OUT18" | head -10
fi
if grep -qE '^  x64.*MISSING' "$OUT18"; then
    pass "and the EXISTING line still shows x64 MISSING (no C++ compiler)"
else
    fail "the existing installed/MISSING line changed unexpectedly"
    sed 's/^/      /' "$OUT18" | head -10
fi
# THE FIX: the root line and the 'builds with' line answer DIFFERENT
# questions (g++ presence vs. what axl-cc will actually compile with), and
# unlabeled they read as one command contradicting itself -- "MISSING"
# directly above "builds with: axl". The root line now names g++ by name so
# a reader sees which compiler each line is about instead of two lines that
# look like they disagree.
if x64_block "$OUT18" | grep -qF 'g++:MISSING'; then
    pass "and the MISSING line names g++, so it reads as answering a different question than 'builds with: axl' rather than contradicting it"
else
    fail "the MISSING line does not name g++ -- still reads as contradicting 'builds with: axl'"
    sed 's/^/      /' "$OUT18" | head -10
fi
# AND THE LINE NOW CARRIES THE C LOCATOR TOO, as an appended FIFTH field.
# Naming g++ stopped the line reading as a contradiction; it did not stop the
# line being HALF an answer. `axl update` carries an arch when EITHER locator
# resolves, so on exactly this fixture `list` reported MISSING about a
# toolchain the update was about to refresh. Field 5 closes that.
C15="$(awk '$1 == "x64" { print $5 }' "$OUT18")"
if [[ "$C15" == "gcc:installed" ]]; then
    pass "and field 5 reports gcc:installed, so list agrees with what update does"
else
    fail "field 5 is '$C15', expected gcc:installed"
    sed 's/^/      /' "$OUT18" | head -10
fi
# APPENDED, NOT INSERTED: case 17 below reads the toolchain ROOT out of field
# 4 with the same awk. A column added anywhere but the end would hand that
# assertion -- the one guarding a command that ends in `rm -rf` -- a compiler
# state string where it expects a path.
R15="$(awk '$1 == "x64" { print $4 }' "$OUT18")"
if [[ "$R15" == "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3" ]]; then
    pass "and field 4 is still the toolchain root, unshifted"
else
    fail "field 4 is '$R15' -- the new column shifted the root reader"
    sed 's/^/      /' "$OUT18" | head -10
fi

echo
echo "=== 16. list: a missing/old axl-cc reports unknown, not a silent blank ==="
setup installed
rm -f "$WORK/opt/axl-sdk-4.6.0/bin/axl-cc"
OUT19="$WORK/no-axlcc.txt"
if AXL toolchain list > "$OUT19" 2>&1; then
    pass "list still exits 0 -- the existing installed/MISSING data is still useful"
else
    fail "list failed entirely just because axl-cc could not answer"
    sed 's/^/      /' "$OUT19" | head -10
fi
# Whole-file: with axl-cc gone, BOTH arches must fall back to 'unknown' --
# an unscoped check is the right one here, it is a claim about the entire
# transcript rather than about one arch's line.
if grep -q 'builds with: unknown' "$OUT19"; then
    pass "and the 'builds with' line names the state as unknown"
else
    fail "no 'unknown' line when axl-cc is missing -- looked silent or confident"
    sed 's/^/      /' "$OUT19" | head -10
fi
if ! grep -q 'builds with: axl' "$OUT19" && ! grep -q 'builds with: host' "$OUT19"; then
    pass "and it never claims a confident axl/host answer it cannot back up"
else
    fail "claimed a confident variant despite axl-cc being unreachable"
    sed 's/^/      /' "$OUT19" | head -10
fi


echo
echo "=== 17. uninstall: the resolved root, proved BEFORE anything can delete ==="
# EVERY uninstall case in THIS file is a REFUSAL. The removal round trip lives
# in test-install-lifecycle.sh, which declares needs=podman and does it inside
# a container. This file declares `needs=` and runs on the developer's own box,
# where the REAL manifest names the /opt toolchain the whole suite compiles
# with -- and that root carries a valid receipt, so a removal assertion here
# would not be refused, it would succeed. Reinstalling is a ~30 minute source
# build.
#
# SAFETY PREFLIGHT -- a hard abort, not a `fail`. What makes the cases below
# safe is that this `axl` resolves the FIXTURE manifest, naming a root under
# $WORK. If a resolution bug ever made it read the machine's real manifest
# instead, the first refusal case would find that valid receipt at
# /opt/x86_64-elf-gcc-* and remove it. `list` resolves the root through the
# same tc_resolve() the uninstall arm uses, so asking it first is a real check
# and not a ritual -- and a `fail` would let the file carry on and do the exact
# thing being guarded against, so this one exits.
setup installed
TCROOT="$WORK/opt/x86_64-elf-gcc-14.3.0-axl3"
AXL toolchain list > "$WORK/preflight.txt" 2>&1
PRE_ROOT="$(awk '$1 == "x64" { print $4 }' "$WORK/preflight.txt")"
if [[ -n "$PRE_ROOT" && "$PRE_ROOT" == "$TCROOT" ]]; then
    pass "preflight: x64 resolves to the fixture root under \$WORK"
else
    fail "preflight: x64 resolved to '$PRE_ROOT', expected '$TCROOT'"
    echo "      ABORTING before any uninstall runs -- see the comment above." >&2
    echo "axl-toolchain-verb: $PASS passed, $FAIL failed"
    exit 1
fi

# THE EXIT CODE IS NOT THE ASSERTION. Before this verb existed the very same
# command exited 2 out of the unknown-subcommand arm, so every rc-only check
# below was green against a tree that had never heard of `uninstall`. Each one
# is therefore paired with the text only the real arm can print.
AXL toolchain uninstall > "$WORK/un-noarg.txt" 2>&1; rc=$?
if [[ $rc -eq 2 ]] && grep -qF 'uninstall: needs an arch (aa64|x64)' "$WORK/un-noarg.txt"; then
    pass "uninstall with no arch exits 2 and names the arches it takes"
else
    fail "bare uninstall: rc=$rc"
    sed 's/^/      /' "$WORK/un-noarg.txt" | head -4
fi
AXL toolchain uninstall mips > "$WORK/un-bogus.txt" 2>&1; rc=$?
if [[ $rc -eq 2 ]] && grep -qF 'uninstall: needs an arch (aa64|x64)' "$WORK/un-bogus.txt"; then
    pass "an unknown arch is refused"
else
    fail "'uninstall mips': rc=$rc"
    sed 's/^/      /' "$WORK/un-bogus.txt" | head -4
fi
# `all` works for `install` and deliberately does not here: `install all`
# retries a download, `uninstall all` deletes two trees from one typo.
AXL toolchain uninstall all > "$WORK/un-all.txt" 2>&1; rc=$?
if [[ $rc -eq 2 ]] && grep -qF "no 'all' here" "$WORK/un-all.txt"; then
    pass "'uninstall all' is refused, and says why there is no 'all'"
else
    fail "'uninstall all': rc=$rc"
    sed 's/^/      /' "$WORK/un-all.txt" | head -4
fi
AXL toolchain uninstall x64 aa64 > "$WORK/un-two.txt" 2>&1; rc=$?
if [[ $rc -eq 2 ]] && grep -qF 'takes one arch' "$WORK/un-two.txt"; then
    pass "a second arch is refused rather than silently ignored"
else
    fail "'uninstall x64 aa64': rc=$rc"
    sed 's/^/      /' "$WORK/un-two.txt" | head -4
fi

echo
echo "=== 18. uninstall: an UNMARKED root is refused, not removed (§21) ==="
# The same ownership rule axl-prune.sh applies, and the same remedy: existing
# toolchains predate receipts, so "not ours" has to come with the way to make
# it ours rather than with a dead end.
setup installed
UN="$WORK/un-noreceipt.txt"
AXL toolchain uninstall x64 > "$UN" 2>&1; rc=$?
if [[ $rc -ne 0 ]] && grep -qF '.axl-receipt' "$UN" && grep -qF "$TCROOT" "$UN"; then
    pass "refuses an unmarked root, naming the receipt wanted and the root looked in"
else
    fail "unmarked root: rc=$rc"
    sed 's/^/      /' "$UN" | head -6
fi
# THE SAFETY HALF, and it is deliberately separate. On its own it is satisfied
# by a crash before the delete just as well as by a correct refusal -- which is
# why the assertion above pins the refusal TEXT. This one pins the consequence,
# and it is the assertion that would scream if the guard ever inverted.
if [[ -d "$TCROOT" ]]; then
    pass "and the refused root is still on disk"
else
    fail "THE REFUSED ROOT WAS REMOVED -- $TCROOT is gone"
fi
if grep -qF 'axl-install-toolchain x64' "$UN"; then
    pass "and the refusal names the re-mark remedy"
else
    fail "the refusal never names 'axl-install-toolchain x64'"
    sed 's/^/      /' "$UN" | head -6
fi

echo
echo "=== 19. uninstall: a receipt is not enough, the RIGHT receipt is ==="
# Same file name, different owner. An SDK root's receipt, or the other arch's,
# must not license this deletion -- the guard is KIND *and* ARCH, the two keys
# install-toolchain.sh writes.
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=sdk
AXL_RECEIPT_ARCH=x64
EOF
UNK="$WORK/un-wrongkind.txt"
AXL toolchain uninstall x64 > "$UNK" 2>&1; rc=$?
if [[ $rc -ne 0 ]] && [[ -d "$TCROOT" ]] && grep -qF "KIND='sdk'" "$UNK"; then
    pass "a receipt with the wrong KIND is refused, and the root survives"
else
    fail "wrong-KIND receipt: rc=$rc root-present=$( [[ -d "$TCROOT" ]] && echo yes || echo NO )"
    sed 's/^/      /' "$UNK" | head -6
fi
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=aa64
EOF
UNA="$WORK/un-wrongarch.txt"
AXL toolchain uninstall x64 > "$UNA" 2>&1; rc=$?
if [[ $rc -ne 0 ]] && [[ -d "$TCROOT" ]] && grep -qF "ARCH='aa64'" "$UNA"; then
    pass "a toolchain receipt for the OTHER arch is refused too"
else
    fail "wrong-ARCH receipt: rc=$rc root-present=$( [[ -d "$TCROOT" ]] && echo yes || echo NO )"
    sed 's/^/      /' "$UNA" | head -6
fi

echo
echo "=== 20. uninstall: a symlinked root is refused, not half-removed ==="
# `rm -rf` on a symlink removes the LINK and leaves the tree, so `list` would
# report MISSING while the bytes stayed on disk -- on x64 that flips the `auto`
# default to host and frees nothing, which is an uninstall that lies.
setup installed
mv "$TCROOT" "$TCROOT-real"
ln -s "$TCROOT-real" "$TCROOT"
UNL="$WORK/un-symlink.txt"
AXL toolchain uninstall x64 > "$UNL" 2>&1; rc=$?
if [[ $rc -ne 0 ]] && [[ -L "$TCROOT" ]] && [[ -d "$TCROOT-real" ]] \
   && grep -qF 'is a symlink' "$UNL"; then
    pass "a symlinked toolchain root is refused, link and tree both intact"
else
    fail "symlinked root: rc=$rc"
    sed 's/^/      /' "$UNL" | head -6
fi

echo
echo "=== 21. uninstall: an arch that is not installed is a no-op, not an error ==="
# `uninstall` is asked for an END STATE, and the state already holds -- the
# same reason `rm -f` exists. A non-zero exit would fail every script that runs
# it twice. It must still SAY so: a silent 0 cannot be told from a removal.
setup
UNN="$WORK/un-absent.txt"
AXL toolchain uninstall x64 > "$UNN" 2>&1; rc=$?
if [[ $rc -eq 0 ]] && grep -qF 'nothing to remove' "$UNN"; then
    pass "uninstalling an absent toolchain exits 0 and says nothing was there"
else
    fail "absent toolchain: rc=$rc"
    sed 's/^/      /' "$UNN" | head -6
fi

echo
echo "=== 22. uninstall targets the SAME root 'list' reports, under an override ==="
# `list` derives the root from AXL_<ARCH>_GXX when it is set (case 9). Two
# copies of that resolution would not merely disagree: `list` would report one
# root installed while `uninstall` removed a different one, and this is the
# command that ends in `rm -rf`. Asserted through the refusal, so nothing is
# deleted to prove it.
setup installed
ALT2="$WORK/alt-root/x86_64-elf-gcc-alt"
mkdir -p "$ALT2/bin"
printf '#!/bin/sh\necho "x86_64-elf-g++ (ALT BUILD) 14.3.0"\n' > "$ALT2/bin/x86_64-elf-g++"
chmod +x "$ALT2/bin/x86_64-elf-g++"
UNO="$WORK/un-override.txt"
AXL_X64_GXX="$ALT2/bin/x86_64-elf-g++" AXL toolchain uninstall x64 > "$UNO" 2>&1; rc=$?
if [[ $rc -ne 0 ]] && grep -qF "$ALT2" "$UNO" && ! grep -qF "$TCROOT" "$UNO"; then
    pass "an overridden compiler moves uninstall's target too, exactly as list"
else
    fail "uninstall ignored AXL_X64_GXX: rc=$rc"
    sed 's/^/      /' "$UNO" | head -6
fi

echo
echo "=== 23. uninstall refuses when the PARENT directory is not writable ==="
# NOT theoretical: measured on this box, /opt is root:root 755 while the
# toolchain root inside it is owned by the user. A plain `rm -rf` descends,
# deletes every byte of the toolchain, and only THEN fails to unlink the top
# directory -- leaving an empty root, no compiler, and an error reported far
# too late to matter. Removing a directory ENTRY needs write on the PARENT, so
# that is what is checked, before anything is removed.
#
# The only case in this file that puts a VALID receipt on the fixture root, so
# it is also the one that proves it is not the ownership guard refusing here.
# Safe for the same reason every other case is: case 17's preflight has already
# pinned the resolved root to $WORK.
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=x64
EOF
chmod a-w "$WORK/opt"
UNW="$WORK/un-nowrite.txt"
AXL toolchain uninstall x64 > "$UNW" 2>&1; rc=$?
chmod u+w "$WORK/opt"
# THE TREE, not the directory. Without the check `rm -rf` empties the root and
# then fails to unlink it, so `rc != 0` and `-d "$TCROOT"` are BOTH still true
# on the broken path -- an assertion built from those two alone passes over a
# destroyed toolchain. The compiler still being there is what discriminates.
if [[ $rc -ne 0 ]] && [[ -x "$TCROOT/bin/x86_64-elf-g++" ]] \
   && grep -qF 'is not writable by' "$UNW"; then
    pass "an unwritable parent is refused with the tree still whole"
else
    fail "unwritable parent: rc=$rc compiler=$( [[ -x "$TCROOT/bin/x86_64-elf-g++" ]] && echo present || echo GONE )"
    sed 's/^/      /' "$UNW" | head -6
fi

echo
echo "=== 24. uninstall: the post-removal line RE-PROBES, it does not assume ==="
# THE BUG THIS CLOSES: the tail used to print a FIXED "x64 now builds with
# the host compiler (auto -> host)" after every successful x64 removal --
# true only when the root just deleted was the ONLY x64 toolchain reachable.
# A SECOND one still reachable via AXL_X64_GCC (a relocated install, a
# developer's own build) means the removal changed nothing, and the old
# fixed line would have said so anyway -- a wrong claim from the verb that
# just ran `rm -rf`. This is the ONE case in the file, besides 23, that lets
# a removal actually SUCCEED (a valid receipt, a writable parent) rather
# than refusing.
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=x64
EOF
ALT3="$WORK/alt-x64-still-here"
mkdir -p "$ALT3/bin"
printf '#!/bin/sh\necho "x86_64-elf-gcc (SURVIVOR) 14.3.0"\n' > "$ALT3/bin/x86_64-elf-gcc"
chmod +x "$ALT3/bin/x86_64-elf-gcc"
UN24="$WORK/un-reprobe.txt"
AXL_X64_GCC="$ALT3/bin/x86_64-elf-gcc" AXL toolchain uninstall x64 > "$UN24" 2>&1; rc24=$?
if [[ $rc24 -eq 0 ]] && grep -qF "removed $TCROOT" "$UN24"; then
    pass "the fixture root was actually removed"
else
    fail "uninstall did not remove $TCROOT: rc=$rc24"
    sed 's/^/      /' "$UN24" | head -8
fi
if grep -qF 'STILL builds with a bare-metal toolchain' "$UN24"; then
    pass "and the tail RE-PROBES: a second reachable compiler is reported correctly, not a fixed 'now builds with host'"
else
    fail "the tail did not report that a second x64 toolchain is still reachable"
    sed 's/^/      /' "$UN24" | head -8
fi
if ! grep -qF 'now builds with the host compiler' "$UN24"; then
    pass "and the OLD fixed claim ('now builds with the host compiler') is not printed"
else
    fail "the tail still claims host unconditionally, contradicting the surviving compiler"
    sed 's/^/      /' "$UN24" | head -8
fi

echo
echo "=== 25. uninstall: the post-removal line reports host when TRUE ==="
# The control for case 24: same removal, no surviving compiler this time, so
# the re-probe SHOULD land on host -- confirming the re-probe is not simply
# hard-coded to always say "still builds with axl" regardless of reality.
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=x64
EOF
UN25="$WORK/un-reprobe-host.txt"
AXL toolchain uninstall x64 > "$UN25" 2>&1; rc25=$?
if [[ $rc25 -eq 0 ]] && grep -qF 'now builds with the host compiler (auto -> host)' "$UN25"; then
    pass "with nothing left reachable, the re-probe correctly reports host"
else
    fail "removal with no surviving x64 compiler did not report host: rc=$rc25"
    sed 's/^/      /' "$UN25" | head -8
fi

echo
echo "=== 26. uninstall --dry-run: runs every guard, reports, and removes nothing ==="
# The other destructive verb in this file (axl prune, axl-prune.sh) has had
# --dry-run since it existed; toolchain uninstall did not, which was the
# asymmetry closed here. Same spelling, same contract: every guard above
# still runs -- arch, path shape, symlink, not-installed, receipt KIND+ARCH,
# parent writability -- and only then does it report instead of renaming or
# removing.
#
# THE ASSERTION IS A NEGATIVE ONE, and case 18's comment already names the
# trap: "the root still exists" is satisfied just as well by a crash before
# the guards run as by a correct dry run. So this pins THREE things: the
# exit code, the exact report text, and -- like case 23 -- the COMPILER
# surviving, not merely the directory. A partial delete leaves the directory
# too; it does not leave a working compiler in it.
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=x64
EOF
UND="$WORK/un-dryrun.txt"
AXL toolchain uninstall --dry-run x64 > "$UND" 2>&1; rcd=$?
if [[ $rcd -eq 0 ]] && grep -qF "would remove $TCROOT" "$UND" \
   && grep -qF 'dry run, nothing removed' "$UND"; then
    pass "dry run exits 0 and reports the resolved root it would remove"
else
    fail "dry run: rc=$rcd"
    sed 's/^/      /' "$UND" | head -8
fi
if [[ -d "$TCROOT" ]] && [[ -x "$TCROOT/bin/x86_64-elf-g++" ]] \
   && [[ -r "$TCROOT/.axl-receipt" ]]; then
    pass "and the root is untouched: directory, compiler and receipt all survive"
else
    fail "DRY RUN REMOVED SOMETHING -- root=$( [[ -d "$TCROOT" ]] && echo present || echo GONE ), compiler=$( [[ -x "$TCROOT/bin/x86_64-elf-g++" ]] && echo present || echo GONE )"
fi

# -n is the short spelling `axl prune -n` also accepts, and it must work
# AFTER the arch too (prune's own flag can land on either side of its
# arguments) -- proving the flag is actually stripped, not just recognized
# when it happens to come first.
UNN2="$WORK/un-dryrun-n.txt"
AXL toolchain uninstall x64 -n > "$UNN2" 2>&1; rcn=$?
if [[ $rcn -eq 0 ]] && grep -qF "would remove $TCROOT" "$UNN2"; then
    pass "'-n' after the arch is accepted, same as '--dry-run' before it"
else
    fail "'-n' short flag: rc=$rcn"
    sed 's/^/      /' "$UNN2" | head -8
fi
if [[ -x "$TCROOT/bin/x86_64-elf-g++" ]]; then
    pass "and the second dry run left the compiler in place too"
else
    fail "the '-n' dry run removed the compiler"
fi

echo
echo "=== 27. list: an EMPTY manifest root still leaves one token in field 4 ==="
# THE SHIFT ARRIVING FROM A VALUE INSTEAD OF A COLUMN. Case 15 guards the root
# reader against someone INSERTING a field before it; this guards it against an
# empty AXL_<ARCH>_TOOLCHAIN_DIR, which collapses to nothing between two spaces
# so awk slides field 5 into field 4 -- and case 17-s root reader, which guards
# a command ending in `rm -rf`, silently receives "gcc:installed" for a path.
setup installed
sed -i 's|^AXL_X64_TOOLCHAIN_DIR=.*|AXL_X64_TOOLCHAIN_DIR=|' \
    "$WORK/opt/axl-sdk-4.6.0/share/axl/axl-toolchains.conf"
OUT27="$WORK/empty-root.txt"
AXL toolchain list > "$OUT27" 2>&1
R27="$(awk '$1 == "x64" { print $4 }' "$OUT27")"
C27="$(awk '$1 == "x64" { print $5 }' "$OUT27")"
if [[ "$R27" == "(unset)" ]]; then
    pass "an empty manifest root prints one placeholder token in field 4"
else
    fail "field 4 is '$R27' -- an empty root shifted the columns"
    sed 's/^/      /' "$OUT27" | head -8
fi
# THE HALF THAT PROVES THE SHIFT WAS THE BUG: without the placeholder this is
# exactly where gcc:installed lands, so asserting it is STILL in field 5 says
# the columns did not move rather than merely that field 4 is non-empty.
if [[ "$C27" == "gcc:installed" || "$C27" == "gcc:MISSING" ]]; then
    pass "and the C-locator state is still in field 5"
else
    fail "field 5 is '$C27' -- the columns moved"
    sed 's/^/      /' "$OUT27" | head -8
fi

echo "axl-toolchain-verb: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
