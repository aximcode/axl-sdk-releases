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

# ── the MACHINE contract ──────────────────────────────────────────────────
#
# `toolchain list --porcelain` is one tab-separated row per arch, and its
# field order is the contract:
#
#   1 arch   2 version   3 root   4 gcc   5 g++   6 builds   7 reason
#
# EVERY VALUE ASSERTION IN THIS FILE READS IT, and that is the point. They
# used to read the HUMAN listing by column index -- `$1 == "x64" {print $4}`
# for the root, $3 for the g++ state, $5 for the C state -- so the rendering
# could not be changed without breaking them, and it had not been: the path
# sat between the two compiler states because a new column could only be
# APPENDED, and three paragraphs of comment in `axl` defended those positions.
# A human format that a machine parses positionally is a human format that is
# frozen. Assertions about the RENDERING still read the rendering; assertions
# about a VALUE read this.
#
# tcf <file> <arch> <field>
tcf() { awk -F'\t' -v a="$2" -v n="$3" '$1 == a { print $n }' "$1"; }

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
# BOTH LOCATORS OF BOTH ARCHES, from the machine contract. Counting the word
# MISSING in the human transcript was the old shape and it counted LINES, not
# facts: it could not tell two absent toolchains from one arch printing the
# word twice, and it broke the moment the human line started saying "not
# installed" -- which is the wording a reader deserves and a parser must not
# depend on.
OUTP="$WORK/list-missing-porcelain.txt"
AXL toolchain list --porcelain > "$OUTP" 2>&1
if [[ "$(tcf "$OUTP" aa64 4)" == "MISSING" && "$(tcf "$OUTP" aa64 5)" == "MISSING" \
   && "$(tcf "$OUTP" x64 4)"  == "MISSING" && "$(tcf "$OUTP" x64 5)"  == "MISSING" ]]; then
    pass "both absent toolchains report MISSING for gcc and g++"
else
    fail "states: aa64 $(tcf "$OUTP" aa64 4)/$(tcf "$OUTP" aa64 5), x64 $(tcf "$OUTP" x64 4)/$(tcf "$OUTP" x64 5)"
    sed 's/^/      /' "$OUTP" | head -4
fi
# ...and the human listing says so in words rather than in a status token.
if grep -qF 'not installed' "$OUT"; then
    pass "and the human listing spells that out as 'not installed'"
else
    fail "the human listing does not say 'not installed'"
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
OUT12P="$WORK/no-override-porcelain.txt"
AXL toolchain list > "$OUT12" 2>&1
AXL toolchain list --porcelain > "$OUT12P" 2>&1
if [[ "$(tcf "$OUT12P" x64 5)" == "MISSING" ]] && ! grep -q 'ALT BUILD' "$OUT12"; then
    pass "control: without the override the same tree reports MISSING"
else
    fail "control failed -- case 9 proves nothing (x64 g++: '$(tcf "$OUT12P" x64 5)')"
fi

echo
echo "=== 9b. --porcelain IS the contract, so its shape is pinned ==="
# WHAT A PARSER IS PROMISED, asserted as a shape rather than assumed by the
# twenty-odd readers above. Every one of them was previously reading the HUMAN
# listing by column index, which is why the rendering could not be improved
# without breaking them; this is the thing that replaced that, so it is the
# thing that now has to be held still.
setup installed
POUT="$WORK/porcelain-shape.txt"
AXL toolchain list --porcelain > "$POUT" 2>&1
prc=$?
if [[ $prc -eq 0 ]]; then
    pass "--porcelain exits 0"
else
    fail "--porcelain exits 0 (got $prc)"
    sed 's/^/      /' "$POUT" | head -6
fi
# EXACTLY two rows, in a fixed order, and NOTHING else -- no banner above and
# no advice below. A trailing "install with:" line is the sort of thing a
# parser must not have to learn to skip, and it is emitted only in the human
# form for exactly that reason.
if [[ "$(wc -l < "$POUT")" -eq 2 ]] \
   && [[ "$(awk -F'\t' 'NR==1{print $1}' "$POUT")" == "aa64" ]] \
   && [[ "$(awk -F'\t' 'NR==2{print $1}' "$POUT")" == "x64" ]]; then
    pass "two rows, aa64 then x64, with no header and no trailing advice"
else
    fail "shape is wrong: $(wc -l < "$POUT") line(s)"
    sed 's/^/      /' "$POUT" | head -6
fi
# SEVEN FIELDS ON EVERY ROW. A field that goes empty -- an unresolvable root,
# a reason nobody filled in -- silently shortens the row and every index after
# it moves, which is the whole failure class this format exists to end.
if [[ "$(awk -F'\t' '{print NF}' "$POUT" | sort -u | tr -d '\n')" == "7" ]]; then
    pass "every row carries exactly 7 tab-separated fields"
else
    fail "field counts seen: $(awk -F'\t' '{print NF}' "$POUT" | sort -u | tr '\n' ' ')"
    cat -A "$POUT" | sed 's/^/      /' | head -4
fi
# NO field may be empty, for the same reason.
if ! awk -F'\t' '{for (i = 1; i <= NF; i++) if ($i == "") exit 1}' "$POUT"; then
    fail "a porcelain field is empty -- use a placeholder, never a gap"
    cat -A "$POUT" | sed 's/^/      /' | head -4
else
    pass "no field is empty"
fi
# The stray-argument refusal applies after the flag too.
if AXL toolchain list --porcelain x64 > "$WORK/pstray.txt" 2>&1; then
    fail "'toolchain list --porcelain x64' accepted a stray argument"
else
    pass "'--porcelain' still rejects a stray argument"
fi

echo
echo "=== 9c. a MALFORMED override must not resolve the root to / ==="
# tc_resolve derives an override-s root as dirname(dirname($_g)), so a value
# one component short -- AXL_X64_GXX=/nonexistent/g++ -- resolves to `/`. The
# listing printed that as the toolchain location, which is an answer that
# reads as true, and the size lookup added beside it then ran `du -sh /` and
# walked the entire filesystem to report the root filesystem-s size as the
# toolchain-s. Measured: 389G, and seconds of I/O, from a status command.
#
# The uninstall arm already refuses exactly this shape by name (empty,
# relative, or `/`) because there it ends in `rm -rf`. list needs it for a
# different reason and needed it just as much.
# THREE SHAPES, because the first guard written here caught only the first of
# them. `/nonexistent/g++` gives `/`; `/usr/bin/g++` -- a locator the update
# arm's own comment calls "an entirely ordinary distro cross-gcc" -- gives
# `/usr`, which IS an absolute existing directory, so it passed and got
# measured with du (11G, reported as the size of a toolchain); and a bare name
# on PATH, which tc_have_compiler accepts on purpose, gives `.`, which the
# placeholder branch printed verbatim and shipped as porcelain field 3, a
# relative path where the contract promises absolute-or-(unset).
setup installed
for _bad in "/nonexistent/g++:/" "/usr/bin/g++:/usr" "g++:."; do
    _loc="${_bad%%:*}"; _would="${_bad##*:}"
    OUT9C="$WORK/bad-override.txt"
    OUT9CP="$WORK/bad-override-porcelain.txt"
    AXL_X64_GXX="$_loc" AXL_X64_GCC="$_loc" AXL toolchain list > "$OUT9C" 2>&1
    AXL_X64_GXX="$_loc" AXL_X64_GCC="$_loc" AXL toolchain list --porcelain > "$OUT9CP" 2>&1
    _got="$(tcf "$OUT9CP" x64 3)"
    if [[ "$_got" == "(unset)" ]]; then
        pass "override $_loc: field 3 is the placeholder, not $_would"
    else
        fail "override $_loc: field 3 is '$_got' (would have been $_would)"
        sed 's/^/      /' "$OUT9CP" | head -4
    fi
    # THE HUMAN LINE, which is where it was actually printed and measured. A
    # POSITIVE assertion on the same transcript, not only the negative one: a
    # crash that renders nothing satisfies "does not print /usr" for free.
    if x64_block "$OUT9C" | grep -qF 'no usable toolchain root'; then
        pass "override $_loc: the human listing says there is no usable root"
    else
        fail "override $_loc: the human listing does not name the state"
        sed 's/^/      /' "$OUT9C" | head -10
    fi
    if ! x64_block "$OUT9C" | grep -qxE " +(/|/usr|\.)"; then
        pass "override $_loc: and names no bogus path as a location"
    else
        fail "override $_loc: the human listing still prints a bogus root"
        sed 's/^/      /' "$OUT9C" | head -10
    fi
done
# The control: a WELL-FORMED override on the same fixture still reports its
# own root, so the guard above rejects malformed values rather than all of
# them. Without this the assertions pass just as well against a list that
# stopped resolving overrides at all.
ALT9="$WORK/alt9/x86_64-elf-gcc-14.3.0-axl3"
mkdir -p "$ALT9/bin"
printf '#!/bin/sh\necho "x86_64-elf-g++ (ALT9) 14.3.0"\n' > "$ALT9/bin/x86_64-elf-g++"
chmod +x "$ALT9/bin/x86_64-elf-g++"
AXL_X64_GXX="$ALT9/bin/x86_64-elf-g++" AXL toolchain list --porcelain > "$WORK/alt9.txt" 2>&1
if [[ "$(tcf "$WORK/alt9.txt" x64 3)" == "$ALT9" ]]; then
    pass "control: a well-formed override still resolves to its own root"
else
    fail "control: field 3 is '$(tcf "$WORK/alt9.txt" x64 3)', expected $ALT9"
    sed 's/^/      /' "$WORK/alt9.txt" | head -4
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
OUT13P="$WORK/builds-with-porcelain.txt"
AXL toolchain list > "$OUT13" 2>&1
AXL toolchain list --porcelain > "$OUT13P" 2>&1
# THE DECISION, from the machine contract: field 6 is the variant, field 7 the
# reason. An exact match, not a substring -- `grep -q 'builds with: axl'` on
# the transcript also matches aa64's fixed line, which is why the block
# helpers had to exist at all.
if [[ "$(tcf "$OUT13P" x64 6)" == "axl" ]]; then
    pass "list: names the variant x64 will actually build with"
else
    fail "porcelain field 6 for x64 is '$(tcf "$OUT13P" x64 6)', expected axl"
    sed 's/^/      /' "$OUT13P" | head -4
fi
if [[ "$(tcf "$OUT13P" x64 7)" == "auto -> axl: bare-metal toolchain installed" ]]; then
    pass "and names the reason: auto -> axl"
else
    fail "porcelain field 7 for x64 is '$(tcf "$OUT13P" x64 7)'"
    sed 's/^/      /' "$OUT13P" | head -4
fi
# ...and the HUMAN line renders that same decision, which is a separate claim:
# the two are computed once and printed twice, and this is what holds the
# second printing honest.
if x64_block "$OUT13" | grep -qF 'builds with: axl -- auto -> axl: bare-metal toolchain installed'; then
    pass "and the human listing renders the same decision on one line"
else
    fail "the human 'builds with' line does not match the porcelain decision"
    sed 's/^/      /' "$OUT13" | head -10
fi

echo
echo "=== 12. list: x64 falls back to host, and says why + the fix ==="
setup installed
OUT14="$WORK/host-fallback.txt"
# Point the C locator at a path that does not exist -- never touch the real
# toolchain under /opt, this is a synthetic tree already.
OUT14P="$WORK/host-fallback-porcelain.txt"
AXL_X64_GCC=/nonexistent/x86_64-elf-gcc AXL toolchain list > "$OUT14" 2>&1
AXL_X64_GCC=/nonexistent/x86_64-elf-gcc AXL toolchain list --porcelain > "$OUT14P" 2>&1
if [[ "$(tcf "$OUT14P" x64 6)" == "host" ]]; then
    pass "list: reports host when the bare-metal C compiler is absent"
else
    fail "porcelain field 6 is '$(tcf "$OUT14P" x64 6)', expected host"
    sed 's/^/      /' "$OUT14P" | head -4
fi
if [[ "$(tcf "$OUT14P" x64 7)" == "auto -> host: no bare-metal toolchain installed" ]]; then
    pass "and names why: auto -> host: no bare-metal toolchain installed"
else
    fail "porcelain field 7 is '$(tcf "$OUT14P" x64 7)'"
    sed 's/^/      /' "$OUT14P" | head -4
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
AXL toolchain list --porcelain > "$OUT15" 2>&1
if [[ "$(tcf "$OUT15" x64 6)" == "axl" ]]; then
    pass "control: without the override, the installed tree says axl"
else
    fail "control failed -- case 12 proves nothing (field 6: '$(tcf "$OUT15" x64 6)')"
    sed 's/^/      /' "$OUT15" | head -4
fi

echo
echo "=== 13. list: aa64 always builds with axl -- no host option exists ==="
setup   # nothing installed at all
OUT16="$WORK/aa64-always-axl.txt"
OUT16P="$WORK/aa64-always-axl-porcelain.txt"
AXL toolchain list > "$OUT16" 2>&1
AXL toolchain list --porcelain > "$OUT16P" 2>&1
if [[ "$(tcf "$OUT16P" aa64 6)" == "axl" \
   && "$(tcf "$OUT16P" aa64 7)" == "aa64 has no host option" ]]; then
    pass "aa64's row states plainly that it has no host option"
else
    fail "aa64 reports '$(tcf "$OUT16P" aa64 6)' / '$(tcf "$OUT16P" aa64 7)'"
    sed 's/^/      /' "$OUT16P" | head -4
fi
if aa64_block "$OUT16" | grep -qF 'builds with: axl -- aa64 has no host option'; then
    pass "and the human listing says so on the aa64 block's own line"
else
    fail "aa64's human 'builds with' line never says it has no host option"
    sed 's/^/      /' "$OUT16" | head -10
fi

echo
echo "=== 14. list: an explicit AXL_TOOLCHAIN pin overrides auto ==="
setup installed
OUT17="$WORK/pinned.txt"
OUT17P="$WORK/pinned-porcelain.txt"
AXL_TOOLCHAIN=host AXL toolchain list > "$OUT17" 2>&1
AXL_TOOLCHAIN=host AXL toolchain list --porcelain > "$OUT17P" 2>&1
if [[ "$(tcf "$OUT17P" x64 6)" == "host" \
   && "$(tcf "$OUT17P" x64 7)" == 'pinned by $AXL_TOOLCHAIN' ]]; then
    pass "x64: an explicit AXL_TOOLCHAIN=host is reported as pinned, not auto"
else
    fail "pin reported as '$(tcf "$OUT17P" x64 6)' / '$(tcf "$OUT17P" x64 7)'"
    sed 's/^/      /' "$OUT17P" | head -4
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
if [[ "$(tcf "$OUT17P" aa64 6)" == "unknown" ]]; then
    pass "aa64: an impossible pin is reported as unknown, not a silent axl"
else
    fail "aa64 reports '$(tcf "$OUT17P" aa64 6)' under an impossible pin"
    sed 's/^/      /' "$OUT17P" | head -4
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
OUT18P="$WORK/gcc-only-porcelain.txt"
AXL toolchain list > "$OUT18" 2>&1
AXL toolchain list --porcelain > "$OUT18P" 2>&1
if [[ "$(tcf "$OUT18P" x64 6)" == "axl" ]]; then
    pass "gcc present + g++ absent: x64 still builds with axl"
else
    fail "x64 reports '$(tcf "$OUT18P" x64 6)' with a working C compiler present"
    sed 's/^/      /' "$OUT18P" | head -4
fi
# BOTH LOCATORS, SEPARATELY, and this fixture is the one that needs them
# apart: `axl update` carries an arch when EITHER resolves, so a listing that
# reported one number for both said MISSING about a toolchain the update was
# about to refresh. Fields 4 and 5 are gcc and g++; the position is the label,
# so the "gcc:"/"g++:" prefixes that disambiguated them in prose are gone.
if [[ "$(tcf "$OUT18P" x64 4)" == "installed" \
   && "$(tcf "$OUT18P" x64 5)" == "MISSING" ]]; then
    pass "and reports gcc installed / g++ MISSING, so list agrees with what update does"
else
    fail "gcc='$(tcf "$OUT18P" x64 4)' g++='$(tcf "$OUT18P" x64 5)', wanted installed/MISSING"
    sed 's/^/      /' "$OUT18P" | head -4
fi
# THE HUMAN RENDERING OF THAT SPLIT, which is the half a reader sees. Two
# lines that answer different questions (is a C++ compiler present vs. what
# will actually compile) used to read as one command contradicting itself:
# a bare "MISSING" directly above "builds with: axl". The state is now spelled
# as what you HAVE, so the two lines cannot be read as disagreeing.
if x64_block "$OUT18" | grep -qF 'gcc only, no C++'; then
    pass "and the human line says 'gcc only, no C++' rather than a bare MISSING"
else
    fail "the human state line does not distinguish gcc-only from missing"
    sed 's/^/      /' "$OUT18" | head -10
fi
# THE ROOT IS STILL THE ROOT, on the fixture whose state words contain spaces.
# This assertion used to read field 4 of the human line and existed to prove a
# newly ADDED column had not shifted it -- a guard that constrained the
# rendering rather than the contract, and one the human layout could not
# survive: "gcc only, no C++" alone puts three tokens where a parser wanted
# one, and the reader that broke is the one deciding where an `rm -rf` points.
# Tab separation is what makes the value's own spaces a non-question.
R15="$(tcf "$OUT18P" x64 3)"
if [[ "$R15" == "$WORK/opt/x86_64-elf-gcc-14.3.0-axl3" ]]; then
    pass "and the root is still field 3, unshifted by a state containing spaces"
else
    fail "field 3 is '$R15' -- expected $WORK/opt/x86_64-elf-gcc-14.3.0-axl3"
    sed 's/^/      /' "$OUT18P" | head -4
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
# THROUGH THE MACHINE CONTRACT, which is what this reader should always have
# used: it decides whether a command ending in `rm -rf` is pointed at $WORK or
# at the real /opt, and it used to take field 4 of a space-separated HUMAN
# line -- a reading that any value containing a space, or any new column,
# silently moves. Field 3 of a tab-separated row cannot be shifted by either.
AXL toolchain list --porcelain > "$WORK/preflight.txt" 2>&1
PRE_ROOT="$(tcf "$WORK/preflight.txt" x64 3)"
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
echo "=== 23b. --dry-run PREVIEWS an unwritable parent, it does not refuse ==="
# A DRY RUN MUST NOT FAIL FOR A CONDITION THAT ONLY AFFECTS THE REAL RUN.
# The writability check ran BEFORE the dry-run report, so on the configuration
# that makes it fire -- /opt root-owned, which is most machines -- `uninstall
# --dry-run` exited 1 saying "cannot remove", having answered none of what was
# asked: what would go, how big it is, how many files. The preview is when you
# most want that, because it is what you read before deciding to type sudo.
#
# `axl prune --dry-run` on the same box previews and exits 0. Two dry runs in
# one program with opposite contracts is a coin flip for the reader.
#
# Still REFUSED under --dry-run: every guard that says the TARGET is wrong
# (bad arch, unusable path, symlink, no receipt, wrong receipt arch). Those
# are not about who is running.
setup installed
cat > "$TCROOT/.axl-receipt" <<EOF
AXL_RECEIPT_KIND=toolchain
AXL_RECEIPT_ARCH=x64
EOF
chmod a-w "$WORK/opt"
UNDW="$WORK/un-dry-nowrite.txt"
AXL toolchain uninstall --dry-run x64 > "$UNDW" 2>&1; rcdw=$?
chmod u+w "$WORK/opt"
if [[ $rcdw -eq 0 ]] && grep -qF "would remove $TCROOT" "$UNDW" \
   && grep -qF 'dry run, nothing removed' "$UNDW"; then
    pass "an unwritable parent still previews, and exits 0"
else
    fail "dry run on an unwritable parent: rc=$rcdw (expected 0 with a preview)"
    sed 's/^/      /' "$UNDW" | head -8
fi
# ...and it says the real run needs root, because that is the next thing the
# reader has to know and the refusal was the only place that used to say it.
#
# KEYED ON WORDING ONLY THE PREVIEW CAN PRODUCE. Grepping for "is not writable
# by" or "sudo" would have PASSED against the refusal being replaced -- the
# text it emits contains both -- so the assertion would have been satisfied by
# the defect. "the real run needs root" appears in no refusal.
if grep -qF 'the real run needs root' "$UNDW"; then
    pass "and names the privilege the real run will need"
else
    fail "the preview does not mention the privilege the real run needs"
    sed 's/^/      /' "$UNDW" | head -8
fi
# THE TREE IS UNTOUCHED, which a preview claiming "nothing removed" has to be
# held to separately -- the claim is text, and text is not evidence. This one
# is a SAFETY NET, not a discriminator: it passes against the code being
# replaced too, because that code removed nothing either. It is here to fail
# the day a dry run starts doing something.
if [[ -x "$TCROOT/bin/x86_64-elf-g++" ]]; then
    pass "and the compiler is still there afterwards"
else
    fail "the dry run removed something"
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
# asymmetry closed here. Same spelling, same contract: every guard that says
# the TARGET is wrong still runs -- arch, path shape, symlink, not-installed,
# receipt KIND+ARCH -- and only then does it report instead of renaming or
# removing. Parent writability is NOT in that list any more (case 23b): it
# says the CALLER lacks a privilege, which is a fact the preview reports
# rather than a reason to refuse to preview.
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
echo "=== 27. list: an EMPTY manifest root is a placeholder, not a gap ==="
# WHY THIS SURVIVES THE MOVE TO --porcelain. It was written against the human
# listing, where an empty AXL_<ARCH>_TOOLCHAIN_DIR collapsed to nothing between
# two spaces and awk slid field 5 into field 4 -- so case 17's root reader,
# which decides where an `rm -rf` points, silently received "gcc:installed"
# for a path. Tab separation removes that failure MODE (an empty field is
# still a field), so this now pins the two things still worth pinning: the
# placeholder is printed rather than an empty field, and the columns after it
# have not moved. The reader it protects has moved to field 3 with it.
setup installed
sed -i 's|^AXL_X64_TOOLCHAIN_DIR=.*|AXL_X64_TOOLCHAIN_DIR=|' \
    "$WORK/opt/axl-sdk-4.6.0/share/axl/axl-toolchains.conf"
OUT27="$WORK/empty-root.txt"
AXL toolchain list --porcelain > "$OUT27" 2>&1
R27="$(tcf "$OUT27" x64 3)"
C27="$(tcf "$OUT27" x64 4)"
if [[ "$R27" == "(unset)" ]]; then
    pass "an empty manifest root prints the (unset) placeholder, not an empty field"
else
    fail "field 3 is '$R27', expected (unset)"
    sed 's/^/      /' "$OUT27" | head -4
fi
if [[ "$C27" == "installed" || "$C27" == "MISSING" ]]; then
    pass "and the C-locator state is still in field 4"
else
    fail "field 4 is '$C27' -- the columns moved"
    sed 's/^/      /' "$OUT27" | head -4
fi

echo "axl-toolchain-verb: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
