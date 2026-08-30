#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-toolchain-prefix.sh -- install-toolchain.sh can target a prefix the
# invoking user owns, and asks for privilege only when it actually needs it.
#
# WHY THIS EXISTS. The script hard-failed without root:
#
#   [install-toolchain] ERROR: extracting to /opt needs root, and
#                       this is not root and has no sudo.
#
# so a user without sudo could not install a cross toolchain at all, and
# without one axl-cc cannot compile anything. That makes the whole root-free
# install story (AXL-Distribution-Design.md §12, P6's `~/.local` root) a
# compiler driver with no compiler. `/opt` was written into the extract, the
# error text and the usage banner.
#
# WHAT THIS DOES NOT NEED TO CHANGE, measured 2026-08-29: USING a relocated
# toolchain already works. AXL_<ARCH>_GCC / _GXX / _BINUTILS_PREFIX are
# resolved by axl-cc, the Makefile and install.sh through the `_DEFAULT`
# convention, and axl-toolchains.conf's own header already names this exact
# case ("a per-user prefix, a CI cache, or a locally built tree"). Only the
# INSTALLER could not put one there.
#
# HOW IT RUNS WITHOUT A 739 MB DOWNLOAD. curl, sha256sum, tar and sudo are
# shimmed on PATH, so the real control flow runs against fakes -- the same
# technique as test-hermetic-toolchain.sh, including its control: a shim that
# is not actually shadowing the real tool makes every assertion below vacuous,
# so that is proved before anything is asserted.
#
# Host-only: no QEMU, no build, no network.
#
# Usage: ./test/integration/test-toolchain-prefix.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
# The subject is exit status and refusal messages, so a non-zero run must be
# one assertion among many rather than an abort.
set +e
set -uo pipefail

INSTALLER="$PROJECT_DIR/scripts/install-toolchain.sh"

WORK="$(mktemp -d -t axl-tcprefix.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== install-toolchain --prefix ==="
echo ""

if [[ ! -x "$INSTALLER" ]]; then
    test_host_fail "scripts/install-toolchain.sh is executable"
    test_host_summary "toolchain-prefix"
    exit 1
fi
test_host_pass "scripts/install-toolchain.sh is executable"

# ── shims ─────────────────────────────────────────────────────
SHIM="$WORK/shim"; mkdir -p "$SHIM"
CALLS="$WORK/calls.log"; : > "$CALLS"

cat > "$SHIM/curl" <<EOF
#!/bin/bash
echo "curl \$*" >> "$CALLS"
# Emit a file wherever -o pointed, so the checksum and extract steps have
# something to work on.
out=""; prev=""
for a in "\$@"; do [[ "\$prev" == "-o" ]] && out="\$a"; prev="\$a"; done
[[ -n "\$out" ]] && echo "not-a-real-tarball" > "\$out"
exit 0
EOF

cat > "$SHIM/sha256sum" <<EOF
#!/bin/bash
echo "sha256sum \$*" >> "$CALLS"
exit 0
EOF

# tar records the -C target and creates a toolchain tree there that the
# script's post-extract check will ACCEPT -- a shim that unpacks nothing would
# make "installed into the prefix" fail for a reason that is the shim's, not
# the installer's. The directory name and version come from the manifest, so
# the fake matches whatever the manifest currently pins.
# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
TC_NAME="$(basename "$AXL_AA64_TOOLCHAIN_DIR")"
TC_VER="${AXL_AA64_TOOLCHAIN_VERSION%%.rel*}"
cat > "$SHIM/tar" <<'TAREOF'
#!/bin/bash
echo "tar $*" >> "__CALLS__"
dest=""; prev=""
for a in "$@"; do [[ "$prev" == "-C" ]] && dest="$a"; prev="$a"; done
if [[ -n "$dest" ]]; then
    echo "$dest" >> "__DESTS__"
    mkdir -p "$dest/__TC_NAME__/bin"
    printf '#!/bin/bash\necho "g++ (fake) __TC_VER__.1"\n' \
        > "$dest/__TC_NAME__/bin/aarch64-none-elf-g++"
    chmod +x "$dest/__TC_NAME__/bin/aarch64-none-elf-g++"
fi
exit 0
TAREOF
sed -i "s|__CALLS__|$CALLS|; s|__DESTS__|$WORK/tar-dests.log|; \
        s|__TC_NAME__|$TC_NAME|g; s|__TC_VER__|$TC_VER|g" "$SHIM/tar"

# sudo records that privilege was requested, then runs the command anyway --
# so a run that should not need root is caught by the LOG, not by a failure.
cat > "$SHIM/sudo" <<EOF
#!/bin/bash
echo "sudo \$*" >> "$CALLS"
exec "\$@"
EOF
chmod +x "$SHIM"/*

# ── control: the shims must actually shadow the real tools ────
#
# Without this, every "sudo was not called" assertion below could mean
# "correct" or "the shim never ran". test-hermetic-toolchain.sh learned this
# the same way.
if PATH="$SHIM:$PATH" curl -o "$WORK/control.bin" http://example.invalid >/dev/null 2>&1 \
        && [[ -f "$WORK/control.bin" ]]; then
    test_host_pass "the shims shadow the real tools (control)"
else
    test_host_fail "the shims shadow the real tools -- every assertion below is vacuous"
    test_host_summary "toolchain-prefix"
    exit 1
fi
# The control just called a shim. Clear the log so it does not pollute the
# "sudo was never called" assertion it exists to make trustworthy.
: > "$CALLS"

# ── --help documents the flag ─────────────────────────────────
HELP="$WORK/help.txt"
"$INSTALLER" --help > "$HELP" 2>&1
if grep -q -- '--prefix' "$HELP"; then
    test_host_pass "--help documents --prefix"
else
    test_host_fail "--help documents --prefix"
    sed 's/^/      /' "$HELP" | head -12
fi
if grep -qi 'root\|sudo' "$HELP"; then
    test_host_pass "--help says when privilege is needed"
else
    test_host_fail "--help says when privilege is needed"
fi

# ── a prefix the user owns needs no privilege at all ──────────
TCDIR="$WORK/mytc"
mkdir -p "$TCDIR"
OUT="$WORK/run.txt"
PATH="$SHIM:$PATH" "$INSTALLER" aa64 --prefix "$TCDIR" > "$OUT" 2>&1
RC=$?
if [[ $RC -eq 0 ]]; then
    test_host_pass "installing into a user-owned prefix succeeds"
else
    test_host_fail "installing into a user-owned prefix succeeds (exit $RC)"
    sed 's/^/      /' "$OUT" | head -8
fi

if grep -q '^sudo ' "$CALLS"; then
    test_host_fail "a writable prefix does NOT escalate"
    grep '^sudo ' "$CALLS" | head -2 | sed 's/^/      /'
else
    test_host_pass "a writable prefix does NOT escalate"
fi

# The extract must land in the prefix the user named, not in /opt.
if grep -qx "$TCDIR" "$WORK/tar-dests.log" 2>/dev/null; then
    test_host_pass "the extract targets the named prefix"
else
    test_host_fail "the extract targets the named prefix"
    echo "      wanted -C $TCDIR"
    sed 's/^/      got: /' "$WORK/tar-dests.log" 2>/dev/null | head -3
fi
if grep -qx "/opt" "$WORK/tar-dests.log" 2>/dev/null; then
    test_host_fail "no extract goes to /opt when --prefix was given"
else
    test_host_pass "no extract goes to /opt when --prefix was given"
fi

# ── the run must tell the user how to USE what it installed ───
#
# The conf's defaults still name /opt, so a toolchain installed anywhere else
# is invisible until AXL_<ARCH>_GCC and friends are set. The installer is the
# only party that knows the prefix, so it is the only one that can say so --
# and an installer that succeeds silently leaves the user with 96 MB on disk
# and a build that still cannot find a compiler.
if grep -q "AXL_AA64_GCC=" "$OUT" && grep -q "$TCDIR" "$OUT"; then
    test_host_pass "the run prints the AXL_AA64_* settings for that prefix"
else
    test_host_fail "the run prints the AXL_AA64_* settings for that prefix"
    sed 's/^/      /' "$OUT" | tail -8
fi

# ── an unwritable prefix must name THAT prefix, not /opt ──────
#
# The old message said "extracting to /opt needs root" unconditionally, which
# is wrong the moment a prefix is a parameter: a reader told the wrong path
# goes looking in the wrong place.
RO="$WORK/readonly"
mkdir -p "$RO"; chmod a-w "$RO"
: > "$CALLS"
ROERR="$WORK/ro.txt"
PATH="$WORK/nosudo:$SHIM:$PATH" env -u SUDO_USER "$INSTALLER" aa64 --prefix "$RO/tc" \
    > "$ROERR" 2>&1
chmod u+w "$RO"
if grep -q -- "$RO/tc" "$ROERR"; then
    test_host_pass "an unwritable prefix is named in the diagnostic"
else
    test_host_fail "an unwritable prefix is named in the diagnostic"
    sed 's/^/      /' "$ROERR" | head -6
fi

echo ""
test_host_summary "toolchain-prefix"
