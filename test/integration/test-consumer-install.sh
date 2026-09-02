#!/bin/bash
# test-meta: arch=x64 needs=podman est=90 local-only=0
# test-consumer-install.sh — the install path, on a machine that has nothing.
#
# WHY THIS EXISTS. AXL-Distribution-Design.md §16 counted what every existing
# check proves and what none of them can: every one runs BEFORE publication, on
# locally built files, on a builder that already has the cross toolchain under
# /opt and the mbedTLS submodule checked out. "Nothing starts from a clean
# machine." A consumer starts from one, and has to get through install.sh
# first.
#
# §16.2 wrote this as "install the built .deb/.rpm with the real package
# manager". D2 retired the packages, so the gate is re-aimed at what replaced
# them: `curl` the installer out of a release directory and run it, exactly as
# README's three commands say to.
#
# TWO DISTRIBUTIONS, FOR A REASON THAT CHANGED. §16 wanted Debian and Fedora
# because one takes .deb and the other .rpm. install.sh is
# distribution-agnostic, so that reason is gone -- but a better one replaced
# it: install.sh is `#!/bin/sh`, and **Debian's /bin/sh is dash while Fedora's
# is bash**. A bashism in a POSIX sh script is invisible on one and fatal on
# the other, and it is the single most likely way this script breaks.
#
# WHAT IS PINNED, none of which any other test can see:
#   - the installer runs on an image with NO compiler, linker or archiver;
#   - it needs only the host commands it declares (`need_cmd`) plus what the
#     image already has -- test-host-deps-minimal.sh proves the SET, this
#     proves install.sh actually gets through with it;
#   - the installed prefix compiles a real .efi there;
#   - every dispatcher command runs, on that machine;
#   - the maintenance verbs work, including uninstall removing what it made.
#
# WHAT IT DELIBERATELY DOES NOT DO. The cross toolchains are MOUNTED, not
# fetched. `axl-install-toolchain all` pulls ~740 MB per generation, which is
# not a per-run cost worth paying on a box that also hosts the CI runner --
# test-host-deps-minimal.sh makes the same trade and says so. What is asserted
# instead is that axl-cc FINDS a mounted toolchain by the absolute path the
# prefix pins, which is the half that can regress.
#
# The SDK tarball is built --arch x64 rather than `all`: this is a test of the
# INSTALL PATH, and test-sdk-tarball.sh already asserts both arches are in the
# shipped archive. Halving the build keeps this affordable.
#
# Usage: ./test/integration/test-consumer-install.sh [--arch X64|AARCH64]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

cd "$PROJECT_DIR" || exit 1

check() {
    if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
    return 0
}

if ! command -v podman >/dev/null 2>&1; then
    echo "SKIP: podman not available"
    exit 0
fi

# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
X64_TC="${AXL_X64_TOOLCHAIN_DIR}"
if [[ ! -d "$X64_TC" ]]; then
    echo "SKIP: x64 toolchain not installed: $X64_TC"
    exit 0
fi

VERSION="$(cat "$PROJECT_DIR/VERSION")"
WORK="$(mktemp -d -t axl-consumer.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
REL="$WORK/rel"; mkdir -p "$REL"

echo "=== consumer install ==="
echo ""

# ---------------------------------------------------------------------------
# A release directory, built the way release.yml builds one. `--base-url` is
# the same seam a mirror would use, so this exercises the real fetch path
# rather than a special "local" mode.
# ---------------------------------------------------------------------------
if ! "$PROJECT_DIR/scripts/make-host-tools-tarball.sh" --out "$REL" > "$WORK/ht.log" 2>&1; then
    test_host_fail "stage a release directory (host-tools)"
    tail -6 "$WORK/ht.log" | sed 's/^/      /'
    test_host_summary "consumer-install"; exit 1
fi
if ! "$PROJECT_DIR/scripts/make-sdk-tarball.sh" --arch x64 --out "$REL" > "$WORK/sdk.log" 2>&1; then
    test_host_fail "stage a release directory (SDK)"
    tail -6 "$WORK/sdk.log" | sed 's/^/      /'
    test_host_summary "consumer-install"; exit 1
fi
cp "$PROJECT_DIR/packaging/install.sh" "$REL/install.sh"
echo "$VERSION" > "$REL/VERSION"
# `sha256sum -- *`, byte for byte what the release job runs. An earlier
# `./*.tar.gz` here wrote "./name" entries and install.sh correctly refused
# them -- a fixture that does not match the producer tests the wrong thing.
( cd "$REL" && sha256sum -- * > SHA256SUMS )
test_host_pass "staged a release directory ($(ls -1 "$REL" | wc -l) assets)"

# ---------------------------------------------------------------------------
# The consumer script. Identical on both images -- if it needs a conditional,
# that is a finding, not a fixture detail.
# ---------------------------------------------------------------------------
CONSUMER='
set -u
export LC_ALL=C
# The image must genuinely lack a toolchain, or the run proves nothing.
for t in gcc g++ ld ar objcopy axl-cc axl; do
    command -v "$t" >/dev/null 2>&1 && { echo "PRECONDITION-FAIL: $t already present"; exit 1; }
done
echo "  precondition: no compiler, linker, archiver or axl on this image"
# Bootstrapping the IMAGE is legitimately distro-specific -- it is the OS
# package manager, not our install path. Everything below this line is
# identical on both, and if it ever needs a conditional that is a finding.
if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq curl xz-utils >/dev/null 2>&1
elif command -v dnf >/dev/null 2>&1; then
    dnf install -y -q curl xz >/dev/null 2>&1
else
    echo "FAIL: no apt-get or dnf on this image"; exit 1
fi
[ $? -eq 0 ] || { echo "FAIL: could not install curl/xz"; exit 1; }

cd /tmp
# Exactly README s first two lines.
curl -fsSLO file:///rel/install.sh || { echo "FAIL: could not fetch install.sh"; exit 1; }
if ! sh install.sh --yes --base-url file:///rel > /tmp/install.log 2>&1; then
    echo "FAIL: install.sh exited $?"; tail -8 /tmp/install.log; exit 1
fi
echo "  PASS install.sh completes on a clean machine"
export PATH="$HOME/.local/bin:$PATH"
rc=0

want() {  # want <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  PASS $1"
    else echo "  FAIL $1: got \"$3\", want \"$2\""; rc=1; fi
}
want "axl --version"    "axl $V"    "$(axl --version 2>&1 | head -1)"
want "axl --print-version" "$V"     "$(axl --print-version 2>&1 | head -1)"

# A real .efi, compiled by the installed axl-cc against the mounted toolchain.
if axl-cc /ex/hello.c -o /tmp/hello.efi >/tmp/cc.log 2>&1; then
    off=$(od -An -tu4 -j60 -N4 /tmp/hello.efi | tr -d " ")
    m=$(od -An -tx2 -j $((off + 4)) -N2 /tmp/hello.efi | tr -d " ")
    want "axl-cc compiles a PE32+ x86-64 image" "8664" "$m"
else
    echo "  FAIL axl-cc could not compile hello.c"; sed "s/^/       /" /tmp/cc.log | tail -4; rc=1
fi

# FOUR SHIPPED COMMANDS ARE PYTHON, and neither of these images has an
# interpreter. install.sh must SAY so -- axl-cc names `rsod-decode` in its
# crash diagnostics, and a user following that advice would otherwise get
# "python3: command not found" with nothing pointing at the cause. It must not
# REFUSE, because axl-cc itself needs no interpreter.
if grep -q "python3 not found" /tmp/install.log; then
    echo "  PASS install.sh advises about the missing python3"
else
    echo "  FAIL install.sh said nothing about python3, which this image lacks"
    rc=1
fi

# Every dispatcher command RUNS. Checked TWICE, because the interesting half is
# what a clean image can do before the advice is followed.
dispatch_check() {  # dispatch_check <label> <expect-python:0|1>
    n=0; bad=""
    for c in $(axl --help | sed -n "s/^  \([a-z][a-z0-9-]*\) .*/\1/p"); do
        n=$((n + 1))
        got=$(axl "$c" --version 2>&1 | head -1)
        case "$got" in "$c $V"|"axl-$c $V") ;; *) bad="$bad $c" ;; esac
    done
    if [ "$n" -le 5 ]; then
        echo "  FAIL dispatcher listed only $n commands"; rc=1; return
    fi
    if [ "$2" = "1" ] && [ -z "$bad" ]; then
        echo "  PASS all $n dispatcher commands run and report $V ($1)"
    elif [ "$2" = "0" ] && [ -n "$bad" ]; then
        # Named, so a NEW failure here is not hidden by the expected four.
        echo "  PASS the non-Python commands run without an interpreter;"
        echo "       needing python3:$bad ($1)"
    else
        echo "  FAIL dispatcher ($1): n=$n failed:$bad"; rc=1
    fi
}
dispatch_check "no python3" 0

# ...and once the advice IS followed, everything works.
if command -v apt-get >/dev/null 2>&1; then apt-get install -y -qq python3 >/dev/null 2>&1
else dnf install -y -q python3 >/dev/null 2>&1; fi
dispatch_check "python3 installed" 1

# The maintenance verbs, on the machine they will actually be used on.
case "$(axl list 2>&1)" in *"* axl-sdk-$V"*) echo "  PASS axl list marks $V current" ;;
                           *) echo "  FAIL axl list: $(axl list 2>&1 | head -2)"; rc=1 ;; esac

# uninstall removes the tree AND the links it made -- the property that makes
# "removal is rm -rf plus the links" true rather than aspirational.
axl uninstall --yes >/tmp/un.log 2>&1
if [ -d "$HOME/.local/share/axl-sdk-$V" ] || [ -e "$HOME/.local/bin/axl-cc" ]; then
    echo "  FAIL axl uninstall left something behind"; ls -a "$HOME/.local/share" "$HOME/.local/bin"; rc=1
else
    echo "  PASS axl uninstall removes the version and its links"
fi

# ...and the host-tools-only install, which ships no compiler at all.
if sh /tmp/install.sh --yes --host-tools --base-url file:///rel > /tmp/ht.log 2>&1; then
    if [ -x "$HOME/.local/share/axl-sdk-host-tools-$V/bin/axl" ]; then
        echo "  PASS --host-tools installs the no-compiler component"
    else
        echo "  FAIL --host-tools produced no bin/axl"; rc=1
    fi
else
    echo "  FAIL --host-tools exited $?"; tail -5 /tmp/ht.log; rc=1
fi
exit $rc
'

run_distro() {  # run_distro <label> <image>
    local label="$1" image="$2"
    local log="$WORK/${label}.log"
    podman run --rm \
        -v "$REL:/rel:ro" \
        -v "$X64_TC:$X64_TC:ro" \
        -v "$PROJECT_DIR/sdk/examples:/ex:ro" \
        -e "V=$VERSION" \
        "$image" sh -c "$CONSUMER" > "$log" 2>&1
    local rc=$?
    sed 's/^/    /' "$log"
    check "$rc" "$label: the consumer's first hour works end to end"
}

# Debian's /bin/sh is dash; Fedora's is bash. That difference is the reason
# there are two, now that neither installs a package.
run_distro "debian(dash)" docker.io/library/debian:stable-slim
run_distro "fedora(bash)" docker.io/library/fedora:latest

test_host_summary "consumer-install"
