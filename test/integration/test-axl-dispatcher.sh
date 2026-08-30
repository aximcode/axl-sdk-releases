#!/bin/bash
# test-meta: arch=none needs= est=12 local-only=0
# test-axl-dispatcher.sh -- the host-side `axl` command dispatcher.
#
# WHY THIS EXISTS. Twelve host-side scripts ship in the SDK and TEN of them are
# on no PATH at all -- only run-qemu and axl-emulate ever got /usr/bin
# wrappers. The consequence is not theoretical: the crash handler prints
#
#     rsod-decode.py --syms <build>/X.so --rsod crash-report.txt
#
# as the command to run, and for anyone who installed a package that is
# `command not found`. test-crashhandler.sh executes that hint -- deliberately,
# because "advice that was never executed" is how it came to name a flag that
# did not exist -- but it strips the program name off the front and substitutes
# its own interpreter, so it proves the ARGUMENTS and structurally cannot see
# that the PROGRAM is unreachable.
#
# `axl <command>` puts one name on PATH and makes the install location stop
# mattering to every printed hint, README and consumer script. See
# AXL-Distribution-Design.md §13.
#
# WHAT IS PINNED. Whole rendered lines, exact. A dispatcher's help IS its
# discovery surface -- §13's whole argument is that `axl --help` is where a
# user finds the ten commands they could not otherwise see -- so a substring
# match would let the listing rot silently.
#
# Host-only: no QEMU. The dispatcher half runs against a SYNTHETIC prefix so it
# needs no build at all; the staging half runs a real `install.sh --prefix`.
#
# Usage: ./test/integration/test-axl-dispatcher.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
# common-test.sh sets -e, which is wrong here: this suite's subject IS exit
# status (an unknown command must exit 2, a failing subcommand must propagate
# its own code), so a non-zero run has to be one assertion among many rather
# than an abort that takes the remaining checks' silence with it.
set +e
set -uo pipefail

AXL_SRC="$PROJECT_DIR/scripts/axl"

WORK="$(mktemp -d -t axl-dispatch.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== axl dispatcher ==="
echo ""

if [[ ! -f "$AXL_SRC" ]]; then
    test_host_fail "scripts/axl exists"
    test_host_summary "axl-dispatcher"
    exit 1
fi
test_host_pass "scripts/axl exists"

norm() { sed -e 's/\x1b\[[0-9;]*m//g' -e 's/[[:space:]]\+/ /g' -e 's/^ //' -e 's/ $//'; }

# assert_line <file> <exact line> <label>
assert_line() {
    if grep -qxF -- "$2" "$1"; then
        test_host_pass "$3"
    else
        test_host_fail "$3"
        echo "      wanted line: $2"
        sed 's/^/      got: /' "$1" | head -8
    fi
}

# ── a SYNTHETIC prefix ────────────────────────────────────────
#
# Two stub commands rather than the real ones: this half is about resolution,
# argv passthrough and exit status, and driving it with run-qemu would make the
# assertions depend on QEMU discovery. The stubs also let the exit code and the
# argument list be chosen to discriminate -- 7 is not a code anything else
# here returns, and the args include one with a space.
PFX="$WORK/pfx"
mkdir -p "$PFX/bin" "$PFX/libexec/axl" "$PFX/share/axl"
cp "$AXL_SRC" "$PFX/bin/axl"; chmod +x "$PFX/bin/axl"
echo "9.9.9" > "$PFX/share/axl/version"

cat > "$PFX/libexec/axl/demo-echo" <<'EOF'
#!/bin/bash
# axl-desc: echo each argument on its own line
for a in "$@"; do echo "arg:$a"; done
EOF
cat > "$PFX/libexec/axl/demo-fail" <<'EOF'
#!/bin/bash
# axl-desc: exit with a distinctive status
exit 7
EOF
# Carries an extension AND the axl- prefix, which is the discriminating case:
# the file must keep its real name (siblings resolve each other by it -- see
# run-qemu.sh's `source "$(dirname "$0")/axl-common.sh"` and profile-qemu.sh's
# `$SCRIPT_DIR/gdb-syms.py`), while the SUBCOMMAND drops both.
cat > "$PFX/libexec/axl/axl-demo-tool.py" <<'EOF'
#!/usr/bin/env python3
# axl-desc: a stub whose filename is not its command name
print("demo-tool ran")
EOF
chmod +x "$PFX/libexec/axl"/demo-* "$PFX/libexec/axl/axl-demo-tool.py"

AXL="$PFX/bin/axl"

# ── --help is the discovery surface ───────────────────────────
HELP="$WORK/help.txt"
"$AXL" --help > "$HELP.raw" 2>&1; HELP_RC=$?
norm < "$HELP.raw" > "$HELP"

if [[ $HELP_RC -eq 0 ]]; then
    test_host_pass "--help exits 0"
else
    test_host_fail "--help exits 0 (got $HELP_RC)"
    sed 's/^/      /' "$HELP" | head -5
fi
assert_line "$HELP" "usage: axl <command> [args ...]" \
    "--help states the usage line"
assert_line "$HELP" "demo-echo echo each argument on its own line" \
    "--help lists a command with the description from its axl-desc header"
assert_line "$HELP" "demo-fail exit with a distinctive status" \
    "--help lists every command, not just the first"
assert_line "$HELP" "demo-tool a stub whose filename is not its command name" \
    "--help lists the COMMAND name, with .py and the axl- prefix stripped"

NOUT=$("$AXL" demo-tool 2>&1)
if [[ "$NOUT" == "demo-tool ran" ]]; then
    test_host_pass "a command resolves to a file whose name differs from it"
else
    test_host_fail "a command resolves to a file whose name differs from it"
    echo "      got: '$NOUT'"
fi

# The three-way distinction. "Tool" is overloaded four ways in this project --
# host-side helper scripts (these), the HOST's own gcc/binutils (which AXL
# deliberately does not use for target code), AXL's bare-metal CROSS toolchain
# under /opt (what actually compiles a .efi), and the target .efi utilities in
# axl-sdk-tools-<arch>.tar.gz. A flat command list invites a reader to assume
# `axl` fronts all of them, so the help says which one it is and names where
# the compiler lives.
assert_line "$HELP" "These commands run on the HOST." \
    "--help says these are host-side commands"
assert_line "$HELP" "To compile for UEFI use axl-cc / axl-c++, which build with AXL's bare-metal" \
    "--help names the compiler drivers rather than implying axl fronts them"
assert_line "$HELP" "cross toolchain, NOT the host's gcc. Install it with axl-install-toolchain." \
    "--help distinguishes the cross toolchain from the host's own gcc"

# A bare `axl` is a discovery moment, not a usage error -- putting the command
# list one keystroke away is the entire point of the dispatcher.
BARE="$WORK/bare.txt"
"$AXL" > "$BARE.raw" 2>&1; BARE_RC=$?
norm < "$BARE.raw" > "$BARE"
if [[ $BARE_RC -eq 0 ]]; then
    test_host_pass "bare axl exits 0"
else
    test_host_fail "bare axl exits 0 (got $BARE_RC)"
fi
if diff -q "$HELP" "$BARE" >/dev/null; then
    test_host_pass "bare axl prints exactly what --help prints"
else
    test_host_fail "bare axl prints exactly what --help prints"
    diff "$HELP" "$BARE" | head -5 | sed 's/^/      /'
fi

# ── the two machine-readable queries ──────────────────────────
#
# AXL-Distribution-Design.md §6 lists `--print-prefix` as MISSING and names the
# cost: "why axl-utils hardcodes /usr/include/axl-sdk". §7 wants a version
# query that is a stable interface rather than a greppable banner, so both are
# asserted as BARE values with nothing else on the line.
VOUT=$("$AXL" --print-version 2>&1)
if [[ "$VOUT" == "9.9.9" ]]; then
    test_host_pass "--print-version emits the bare version and nothing else"
else
    test_host_fail "--print-version emits the bare version and nothing else"
    echo "      got: '$VOUT'"
fi

POUT=$("$AXL" --print-prefix 2>&1)
if [[ "$POUT" == "$PFX" ]]; then
    test_host_pass "--print-prefix emits the resolved prefix"
else
    test_host_fail "--print-prefix emits the resolved prefix"
    echo "      wanted: '$PFX'"
    echo "      got:    '$POUT'"
fi

# ── dispatch: argv passthrough and exit status ────────────────
#
# An argument containing a space is the discriminating case: a dispatcher that
# forwards with $* or an unquoted $@ splits it, and every simple test would
# still pass.
EOUT="$WORK/echo.txt"
"$AXL" demo-echo one "two three" --flag=x > "$EOUT" 2>&1
ECHO_RC=$?
if [[ $ECHO_RC -eq 0 ]]; then
    test_host_pass "a subcommand runs"
else
    test_host_fail "a subcommand runs (exit $ECHO_RC)"
    sed 's/^/      /' "$EOUT" | head -5
fi
assert_line "$EOUT" "arg:one" "argv passthrough keeps a plain argument"
assert_line "$EOUT" "arg:two three" "argv passthrough keeps a quoted argument intact"
assert_line "$EOUT" "arg:--flag=x" "argv passthrough keeps a flag-shaped argument"
if [[ "$(wc -l < "$EOUT")" -eq 3 ]]; then
    test_host_pass "argv passthrough adds no arguments of its own"
else
    test_host_fail "argv passthrough adds no arguments of its own"
    sed 's/^/      /' "$EOUT"
fi

"$AXL" demo-fail >/dev/null 2>&1
if [[ $? -eq 7 ]]; then
    test_host_pass "a subcommand's exit status propagates"
else
    test_host_fail "a subcommand's exit status propagates"
fi

# ── an unknown command must say so, and name the way out ──────
UERR="$WORK/unknown.txt"
"$AXL" no-such-command >"$UERR.raw" 2>&1; UERR_RC=$?
norm < "$UERR.raw" > "$UERR"
if [[ $UERR_RC -eq 2 ]]; then
    test_host_pass "an unknown command exits 2"
else
    test_host_fail "an unknown command exits 2 (got $UERR_RC)"
fi
assert_line "$UERR" "axl: unknown command 'no-such-command'" \
    "an unknown command is named back to the user"
assert_line "$UERR" "Run 'axl --help' for the list of commands." \
    "an unknown command names the way to the list"

# ── staging: install.sh must actually produce all of this ─────
#
# The half above proves the dispatcher works when the layout is right. This
# proves install.sh BUILDS that layout -- without it the dispatcher is correct
# and absent, which is the same failure as a gate outside the gate runner.
echo ""
echo "-- install.sh stages the dispatcher --"
IPFX="$WORK/installed"
if ! "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$IPFX" \
        > "$WORK/install.log" 2>&1; then
    test_host_fail "install.sh --prefix succeeds"
    tail -5 "$WORK/install.log" | sed 's/^/      /'
else
    test_host_pass "install.sh --prefix succeeds"

    if [[ -x "$IPFX/bin/axl" ]]; then
        test_host_pass "install.sh stages bin/axl executable"
    else
        test_host_fail "install.sh stages bin/axl executable"
    fi

    # Staged under their REAL filenames, because these scripts resolve each
    # other by name from their own directory -- run-qemu.sh sources
    # axl-common.sh, profile-qemu.sh runs $SCRIPT_DIR/gdb-syms.py, axl-emulate
    # execs run-qemu.sh from Path(__file__).parent. Renaming them in libexec
    # would leave each one correct and unable to find the others.
    for f in rsod-decode.py run-qemu.sh axl-emulate axl-common.sh; do
        if [[ -e "$IPFX/libexec/axl/$f" ]]; then
            test_host_pass "libexec/axl/$f is staged under its real name"
        else
            test_host_fail "libexec/axl/$f is staged under its real name"
        fi
    done

    # ...and reachable under their COMMAND names. This is the pair that
    # matters: the file keeps the name its siblings expect, the user types the
    # name the dispatcher advertises.
    for cmd in rsod-decode run-qemu emulate; do
        if "$IPFX/bin/axl" "$cmd" --help >/dev/null 2>&1; then
            test_host_pass "axl $cmd resolves and runs"
        else
            test_host_fail "axl $cmd resolves and runs"
        fi
    done

    # End to end against the real staged tree: the version must come from the
    # tree's own VERSION, not the synthetic 9.9.9 above.
    WANT_VER=$(cat "$PROJECT_DIR/VERSION")
    GOT_VER=$("$IPFX/bin/axl" --print-version 2>&1)
    if [[ "$GOT_VER" == "$WANT_VER" ]]; then
        test_host_pass "the staged axl reports the tree's version ($WANT_VER)"
    else
        test_host_fail "the staged axl reports the tree's version"
        echo "      wanted: '$WANT_VER'  got: '$GOT_VER'"
    fi

    # The listing must show the REAL commands, which is what makes `axl --help`
    # a discovery surface rather than a synthetic-fixture artefact.
    RHELP="$WORK/real-help.txt"
    "$IPFX/bin/axl" --help 2>&1 | norm > "$RHELP"
    if grep -qE '^rsod-decode .+' "$RHELP"; then
        test_host_pass "the staged --help lists rsod-decode with a description"
    else
        test_host_fail "the staged --help lists rsod-decode with a description"
        sed 's/^/      /' "$RHELP" | head -8
    fi

    # ...and the support files must NOT be offered. axl-common.sh is SOURCED
    # by run-qemu.sh; gdb-sample.py is loaded INSIDE gdb and has no shebang at
    # all. Either one listed is a command a user would reasonably try and that
    # cannot run -- and gdb-sample.py is 755 in the tree, so a staging rule
    # keyed on the mode bit alone would offer exactly that.
    # NOTE the names: cmd_name() strips the leading `axl-`, so axl-common.sh
    # would be offered as `common`, not `axl-common`. The first draft grepped
    # for `axl-common` -- a pattern that can never match, i.e. an assertion
    # that asserts nothing. The sabotage run is what exposed it.
    for hidden in common gdb-sample; do
        if grep -qE "^$hidden( |\$)" "$RHELP"; then
            test_host_fail "$hidden is not offered as a command"
            grep -E "^$hidden" "$RHELP" | sed 's/^/      /'
        else
            test_host_pass "$hidden is not offered as a command"
        fi
    done
fi

echo ""
test_host_summary "axl-dispatcher"
