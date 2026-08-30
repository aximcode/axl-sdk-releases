#!/bin/bash
# test-meta: arch=none needs= est=14 local-only=0
# test-sdk-selfcontained.sh -- an SDK prefix owns everything it needs, so
# `rm -rf <prefix>` is a COMPLETE uninstall.
#
# WHY THIS EXISTS. This is the contract every pruning policy rests on.
# AXL-Distribution-Design.md §12.2 makes the install root versioned
# (/opt/axl-sdk-<ver>) and §12.3 prunes old ones; a consumer on the tarball
# path prunes by deleting a directory. All of that is only safe if a prefix
# writes nothing outside itself -- otherwise deleting one leaves droppings, or
# worse, deleting one breaks another.
#
# It was believed true and asserted NOWHERE, which is how it would stop being
# true: one `install -m 644 ... "$HOME/.config/..."` in install.sh and the
# guarantee is silently gone while every other test stays green. A "safe to
# delete" claim that is not tested is the kind that becomes false in one
# commit -- and it is quoted at consumers, so it has to be a contract rather
# than an implementation detail.
#
# WHAT IS ALLOWED OUT. Exactly one thing: the bare-metal cross toolchains,
# which live outside the prefix on purpose (739 MB shared across every SDK
# version, and installed separately by axl-install-toolchain). Their paths come
# from the manifest, so the allowlist is READ from it rather than restated --
# a hardcoded /opt here would pass against a relocated toolchain and would be
# its own drift.
#
# Host-only: no QEMU, no network.
#
# Usage: ./test/integration/test-sdk-selfcontained.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

WORK="$(mktemp -d -t axl-selfcont.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "=== SDK prefix is self-contained ==="
echo ""

# ── install with a HOME of our own ────────────────────────────
#
# An empty HOME that nothing else can touch is how "wrote nothing outside the
# prefix" becomes checkable at all: anything the install leaves behind for the
# USER lands here, and the directory is created empty so a single new entry is
# a failure with a name attached.
FAKE_HOME="$WORK/home"
PREFIX="$WORK/pfx"
mkdir -p "$FAKE_HOME"

if HOME="$FAKE_HOME" XDG_CONFIG_HOME="$FAKE_HOME/.config" \
   XDG_CACHE_HOME="$FAKE_HOME/.cache" XDG_DATA_HOME="$FAKE_HOME/.local/share" \
   "$PROJECT_DIR/scripts/install.sh" --arch x64 --prefix "$PREFIX" \
        > "$WORK/install.log" 2>&1; then
    test_host_pass "install.sh --prefix succeeds"
else
    test_host_fail "install.sh --prefix succeeds"
    tail -10 "$WORK/install.log" | sed 's/^/      /'
    test_host_summary "sdk-selfcontained"
    exit 1
fi

# Positive control: the prefix must actually have been populated, or "nothing
# outside it" is trivially true of an install that did nothing.
if [[ -x "$PREFIX/bin/axl-cc" && -f "$PREFIX/lib/axl/x64/libaxl.a" ]]; then
    test_host_pass "the prefix is populated (control)"
else
    test_host_fail "the prefix is populated -- the assertions below are vacuous"
    test_host_summary "sdk-selfcontained"
    exit 1
fi

STRAY="$(find "$FAKE_HOME" -mindepth 1 2>/dev/null)"
if [[ -z "$STRAY" ]]; then
    test_host_pass "the install wrote nothing into HOME"
else
    test_host_fail "the install wrote nothing into HOME"
    echo "$STRAY" | head -5 | sed 's/^/      /'
fi

# ── the generated files must not point outside ────────────────
#
# Only the machine-readable ones that ENCODE paths for a build are checked.
# The shell drivers and the headers are full of prose with slashes in it, and
# grepping those for "absolute paths" matches "/2/4-byte" and "/AESDEC" -- a
# check that noisy gets suppressed rather than fixed.
#
# The allowlist is the two toolchain directories, read from the manifest the
# install just staged.
# shellcheck source=/dev/null
. "$PREFIX/share/axl/axl-toolchains.conf"
TC_AA64="$AXL_AA64_TOOLCHAIN_DIR"
TC_X64="$AXL_X64_TOOLCHAIN_DIR"
if [[ -n "$TC_AA64" && -n "$TC_X64" ]]; then
    test_host_pass "the staged manifest names both toolchain roots"
else
    test_host_fail "the staged manifest names both toolchain roots"
fi

for rel in lib/cmake/axl/axl-config.cmake lib/pkgconfig/axl.pc \
           share/axl/axl-toolchains.conf; do
    f="$PREFIX/$rel"
    if [[ ! -f "$f" ]]; then
        test_host_fail "$rel is present"
        continue
    fi
    # Extracting "an absolute path" is where the first draft of this went
    # wrong -- a bare /[A-Za-z...] match reported /axl.aximcode.com out of a
    # URL, /include/axl-sdk out of "${AXL_SDK_DIR}/include/axl-sdk", and
    # /AESDEC out of a comment. A check that noisy gets suppressed rather than
    # fixed. So: drop comments, drop lines carrying a URL, and require the
    # slash NOT to follow a word char, a dot, `}` or `)` -- which is what
    # separates a real absolute path from a variable-relative one and from a
    # hostname.
    BAD="$(sed -e 's/#.*//' -e '/:\/\//d' "$f" \
           | grep -oP '(?<![A-Za-z0-9_.}\)])/[A-Za-z0-9_][A-Za-z0-9_/.+-]{3,}' \
           | sort -u \
           | grep -v "^$PREFIX" \
           | grep -v "^$TC_AA64" \
           | grep -v "^$TC_X64" \
           | grep -vE '^/(usr|bin|sbin|lib|lib64|etc|dev|proc|sys|tmp|var)(/|$)' \
           || true)"
    if [[ -z "$BAD" ]]; then
        test_host_pass "$rel points only at the prefix, the toolchains or the system"
    else
        test_host_fail "$rel points only at the prefix, the toolchains or the system"
        echo "$BAD" | head -5 | sed 's/^/      stray: /'
    fi
done

# ── therefore: deleting the directory uninstalls it ───────────
#
# Stated as an assertion rather than left as an inference, because this is the
# sentence consumers are told and the one §12.3's prune depends on.
rm -rf "$PREFIX"
LEFT="$(find "$FAKE_HOME" -mindepth 1 2>/dev/null)"
if [[ ! -e "$PREFIX" && -z "$LEFT" ]]; then
    test_host_pass "removing the prefix is a complete uninstall"
else
    test_host_fail "removing the prefix is a complete uninstall"
    echo "$LEFT" | head -5 | sed 's/^/      left behind: /'
fi

echo ""
test_host_summary "sdk-selfcontained"
