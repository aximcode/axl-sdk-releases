#!/bin/bash
# test-meta: arch=x64 needs=podman est=150 local-only=0
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
# ...with ONE exception, and it is a whole third arm: `ubuntu(host gcc)` mounts
# NO toolchain, installs only the distro's gcc/binutils, and asserts that /opt
# is empty and nothing named x86_64-elf-* exists anywhere in the image BEFORE
# it builds. That is the one claim no other check in this tree can make --
# every other AXL_TOOLCHAIN=host test runs on a box that HAS the toolchain and
# simulates absence with a bad locator. See the arm's own comment and
# AXL-Host-Toolchain-Design.md §7.3.
#
# ...AND A FOURTH ARM CLOSING THE GAP THOSE TWO LEAVE: `ubuntu(both)` mounts
# the bare-metal toolchain AND installs the distro's gcc/binutils, so BOTH are
# genuinely on this machine -- the `auto -> axl` direction, which is the more
# dangerous one to get wrong. If `auto`'s presence probe ever regressed to
# reporting "absent" unconditionally, a user with both installed would
# silently build with the HOST compiler and succeed: the wrong toolchain, a
# green build, no diagnostic. Neither of the other two arms can catch that --
# `debian(dash)`/`fedora(bash)` have no host gcc to fall back to, so a broken
# probe would fail loudly there instead, which is a different (and safe)
# outcome; `ubuntu(host gcc)` has no toolchain to resolve TO. The one existing
# assertion of `auto -> axl` (test-host-toolchain-qemu.sh:324) runs on THIS
# development box, which also has both -- but this box is not a fresh install:
# it carries a long-lived `stage/`, a populated PATH and an existing manager
# root, exactly what a container rules out. See the arm's own comment.
#
# The SDK tarball is built --arch x64 rather than `all`: this is a test of the
# INSTALL PATH, and test-sdk-tarball.sh already asserts both arches are in the
# shipped archive. Halving the build keeps this affordable.
#
# SKIP BALANCE: every skip in this file is a whole-file `exit 0` taken before
# any assertion is tallied (no podman / no staged x64 toolchain to build the
# release directory from), so all four arms report zero either way and adding
# one cannot unbalance the count.
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

# AND A C++ ONE, which is a strictly larger ask of the install than C is.
# axl-c++ has to find the toolchain-s libstdc++/libsupc++, the four
# axl-cxxrt-*.o glue objects and the EXCEPTIONS linker script -- none of which
# hello.c touches. Until this, the only evidence they shipped correctly was
# test-sdk-tarball.sh asserting `bin/axl-c++` and hello.cpp exist BY NAME: a
# file listing cannot tell you they link. Every real C++ test in this suite
# builds in-tree, against a source checkout, which is the one caller whose
# success proves least about a consumer install.
if axl-c++ /ex/hello.cpp -o /tmp/hellopp.efi >/tmp/cxx.log 2>&1; then
    off=$(od -An -tu4 -j60 -N4 /tmp/hellopp.efi | tr -d " ")
    m=$(od -An -tx2 -j $((off + 4)) -N2 /tmp/hellopp.efi | tr -d " ")
    want "axl-c++ compiles a PE32+ x86-64 image" "8664" "$m"
else
    echo "  FAIL axl-c++ could not compile hello.cpp"; sed "s/^/       /" /tmp/cxx.log | tail -6; rc=1
fi
# EXCEPTIONS are the part most likely to be missing from a staged tree rather
# than merely broken: they need the linker script AND the unwinder, and they
# are a per-TU opt-in, so nothing else in this run would notice their absence.
if axl-c++ -fexceptions /ex/cxx-errors.cpp -o /tmp/exc.efi >/tmp/exc.log 2>&1; then
    off=$(od -An -tu4 -j60 -N4 /tmp/exc.efi | tr -d " ")
    m=$(od -An -tx2 -j $((off + 4)) -N2 /tmp/exc.efi | tr -d " ")
    want "axl-c++ -fexceptions links a PE32+ image" "8664" "$m"
else
    echo "  FAIL axl-c++ -fexceptions could not link"; sed "s/^/       /" /tmp/exc.log | tail -6; rc=1
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

# ---------------------------------------------------------------------------
# THE HOST-COMPILER ARM: a machine where the bare-metal toolchain does not
# EXIST, rather than one where it merely goes unused.
#
# Every other check of AXL_TOOLCHAIN=host in this tree runs on THIS box, which
# has the toolchain under /opt, and simulates absence by pointing a locator at
# a path that is not there. That proves the code takes the fall-back branch.
# It does not prove a consumer with no toolchain can build, because the branch
# it takes could still be reaching something the box happens to have.
# "Unused" and "absent" are different claims and only a container proves the
# second (AXL-Host-Toolchain-Design.md §7.3).
#
# So the value of this arm is ENTIRELY in its control: /opt is empty and
# nothing named x86_64-elf-* exists anywhere in the image, asserted BEFORE a
# single byte is compiled, so a green build below cannot be explained by a
# toolchain that was quietly present. Re-asserted AFTER install.sh runs, since
# the installer is the one thing in the run that could plausibly go and fetch
# one (`--toolchain` does exactly that; this invocation must not).
#
# Ubuntu 24.04 because it is what the driving consumer builds on, and its gcc
# 13.3 is the compiler the feasibility spike measured (design §2).
# ---------------------------------------------------------------------------
CONSUMER_HOSTGCC='
set -u
export LC_ALL=C
rc=0

want() {  # want <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  PASS $1"
    else echo "  FAIL $1: got \"$3\", want \"$2\""; rc=1; fi
}

# A fresh machine, not a re-run over a half-installed prefix.
for t in axl-cc axl; do
    command -v "$t" >/dev/null 2>&1 && { echo "PRECONDITION-FAIL: $t already present"; exit 1; }
done

# no_bare_metal_toolchain <when> -- THE CONTROL. Three independent statements
# of the same absence, because each can be true while another is false:
# a directory can be empty while a binary sits on PATH from elsewhere, and a
# read-only bind mount at an absolute path satisfies neither a PATH lookup nor
# a stat of the configured prefix.
#
# The scan is a real filesystem walk rather than a PATH lookup, and it does
# NOT use -xdev: a mounted-in toolchain is exactly the state this arm exists
# to rule out, and -xdev would step over the mount that carried it.
#
# Each check separates "the tool could not RUN" from "the tool ran and found
# NOTHING" -- the same empty output, opposite facts -- by capturing the exit
# status immediately and never reading the output alone.
no_bare_metal_toolchain() {
    _when="$1"

    if command -v x86_64-elf-gcc >/dev/null 2>&1; then
        echo "  FAIL control ($_when): x86_64-elf-gcc IS on PATH at $(command -v x86_64-elf-gcc)"
        rc=1
    else
        echo "  PASS control ($_when): x86_64-elf-gcc is on no PATH entry"
    fi

    if [ ! -d /opt ]; then
        echo "  PASS control ($_when): /opt does not exist"
    else
        ls -A /opt > /tmp/opt.txt 2>/tmp/opt.err
        _lrc=$?
        if [ "$_lrc" -ne 0 ]; then
            echo "  FAIL control ($_when): could not list /opt (exit $_lrc)"
            sed "s/^/       /" /tmp/opt.err | head -3; rc=1
        elif [ -s /tmp/opt.txt ]; then
            echo "  FAIL control ($_when): /opt is NOT empty:"
            sed "s/^/       /" /tmp/opt.txt | head -5; rc=1
        else
            echo "  PASS control ($_when): /opt is empty"
        fi
    fi

    find / -path /proc -prune -o -path /sys -prune -o -path /dev -prune -o \
           -name "x86_64-elf-*" -print > /tmp/scan.txt 2>/tmp/scan.err
    _frc=$?
    if [ "$_frc" -ne 0 ]; then
        echo "  FAIL control ($_when): the filesystem scan itself failed (exit $_frc)"
        sed "s/^/       /" /tmp/scan.err | head -3; rc=1
    elif [ -s /tmp/scan.txt ]; then
        echo "  FAIL control ($_when): a bare-metal toolchain IS in this image:"
        sed "s/^/       /" /tmp/scan.txt | head -5; rc=1
    else
        echo "  PASS control ($_when): nothing named x86_64-elf-* anywhere in the image"
    fi
}

no_bare_metal_toolchain "before install"
echo "  host compiler: $(gcc --version | sed -n 1p) / $(ld --version | sed -n 1p)"

cd /tmp
curl -fsSLO file:///rel/install.sh || { echo "FAIL: could not fetch install.sh"; exit 1; }
if ! sh install.sh --yes --base-url file:///rel > /tmp/install.log 2>&1; then
    echo "FAIL: install.sh exited nonzero"; tail -8 /tmp/install.log; exit 1
fi
echo "  PASS install.sh completes on a machine with no cross toolchain"
export PATH="$HOME/.local/bin:$PATH"

# The installer had its chance to drag one in; it must not have taken it.
no_bare_metal_toolchain "after install"

want "axl --version" "axl $V" "$(axl --version 2>&1 | head -1)"

# AXL_TOOLCHAIN is deliberately UNSET: `auto` is what a consumer gets, and the
# claim under test is that it resolves by probing, on a machine where the
# probe can only answer one way. Setting host explicitly would test the
# variant instead of the resolution.
want "AXL_TOOLCHAIN=auto resolves to host" "host" "$(axl-cc --print-toolchain 2>&1 | head -1)"

# build_efi <label> <src> <out> -- a zero exit is not the assertion. axl-cc
# has to have produced a real PE32+ x86-64 image, so the check reads the
# file: the PE header offset at 0x3c, then the COFF machine word after the
# signature.
build_efi() {
    if ! axl-cc "$2" -o "$3" > /tmp/cc.log 2>&1; then
        echo "  FAIL $1: axl-cc exited nonzero"
        sed "s/^/       /" /tmp/cc.log | tail -8; rc=1; return
    fi
    if [ ! -s "$3" ]; then
        echo "  FAIL $1: axl-cc exited 0 and produced no file"; rc=1; return
    fi
    off=$(od -An -tu4 -j60 -N4 "$3" | tr -d " ")
    m=$(od -An -tx2 -j $((off + 4)) -N2 "$3" | tr -d " ")
    if [ "$m" = "8664" ]; then
        echo "  PASS $1 ($(wc -c < "$3" | tr -d " ") bytes, PE32+ x86-64)"
    else
        echo "  FAIL $1: not a PE32+ x86-64 image (machine word $m)"; rc=1
    fi
}

# hello.c is the floor. http-server.c is NOT optional: a simple source links
# with no libc at all because --gc-sections drops the vendored code (libvterm,
# lzma, mbedTLS) that references the plain leaf names, so it would pass under
# a standin that supplied nothing. http-server is the shipped source that
# KEEPS them, and it is what exposed "no libc is needed" as wrong (§7.2).
build_efi "the host gcc builds hello.c"       /ex/hello.c       /tmp/hello.efi
build_efi "the host gcc builds http-server.c" /ex/http-server.c /tmp/http-server.efi

exit $rc
'

# NO -v "$X64_TC:$X64_TC:ro". run_distro mounts the host toolchain because its
# arms assert axl-cc finds one by the absolute path the prefix pins; this arm
# exists to remove exactly that mount, so it is a sibling rather than a flag on
# run_distro. The bootstrap installs gcc + binutils (the subject) and curl
# (install.sh's own `need_cmd`; tar, sha256sum, awk and tr are already in the
# base image, and the assets are .tar.gz so nothing needs xz).
run_distro_hostgcc() {  # run_distro_hostgcc <label> <image> <bootstrap-cmd>
    local label="$1" image="$2" bootstrap="$3"
    local log="$WORK/${label}.log"
    podman run --rm \
        -v "$REL:/rel:ro" \
        -v "$PROJECT_DIR/sdk/examples:/ex:ro" \
        -e "V=$VERSION" \
        "$image" sh -c "$bootstrap
$CONSUMER_HOSTGCC" > "$log" 2>&1
    local rc=$?
    sed 's/^/    /' "$log"
    # A zero exit is not enough on its own: a container that produced no
    # output at all -- a shell that never reached the script, an image whose
    # sh is not there -- would also exit 0, and this arm's whole value is in
    # assertions it PRINTS. So require the count it is contracted to print:
    # 11 as of 2026-09-03 (6 control lines, install.sh, --version, the
    # resolution, and the two builds). `|| true` because `grep -c` prints 0
    # and exits 1 on no match, and an empty count would abort the arithmetic.
    local npass
    npass=$(grep -c "^  PASS " "$log" || true)
    if [[ "$rc" -eq 0 && "$npass" -lt 11 ]]; then
        echo "    the arm printed only $npass PASS lines, fewer than the 11 it promises"
        rc=1
    fi
    check "$rc" "$label: builds x64 with the distro's own gcc, no toolchain in the image"
}

run_distro_hostgcc "ubuntu(host gcc)" docker.io/library/ubuntu:24.04 \
    '{ apt-get update -qq && apt-get install -y -qq gcc binutils curl; } > /tmp/apt.log 2>&1 \
        || { echo "FAIL: could not apt-get gcc/binutils/curl"; tail -8 /tmp/apt.log; exit 1; }'

# ---------------------------------------------------------------------------
# THE BOTH-PRESENT ARM: a machine where the Ubuntu-packaged gcc AND the
# bare-metal toolchain are BOTH installed. Neither run_distro (mounts the
# toolchain, installs no gcc) nor run_distro_hostgcc (installs gcc, mounts no
# toolchain) can reach this state -- see the file header for why it is the
# more dangerous direction to leave unguarded.
#
# `AXL_TOOLCHAIN` is deliberately left UNSET: `auto` is what a consumer with
# nothing pinned gets, and asserting a variant that was EXPLICITLY selected
# would test the selection, not the probe.
# ---------------------------------------------------------------------------
CONSUMER_BOTH='
set -u
export LC_ALL=C
rc=0

want() {  # want <label> <expected> <actual>
    if [ "$2" = "$3" ]; then echo "  PASS $1"
    else echo "  FAIL $1: got \"$3\", want \"$2\""; rc=1; fi
}

# A fresh machine, not a re-run over a half-installed prefix.
for t in axl-cc axl; do
    command -v "$t" >/dev/null 2>&1 && { echo "PRECONDITION-FAIL: $t already present"; exit 1; }
done

# 1. BOTH compilers genuinely present, asserted BEFORE any build. The
# version line proves the Ubuntu gcc is the DISTRO package, not ours; running
# the mounted compiler (not just stat-ing it, per the same usability-not-
# existence reasoning axl-cc itself applies) proves it actually works.
if command -v gcc >/dev/null 2>&1; then
    echo "  PASS control: Ubuntu gcc on PATH ($(gcc --version | sed -n 1p))"
else
    echo "  FAIL control: no gcc on PATH"; rc=1
fi

TC_GCC="$X64_TC/bin/x86_64-elf-gcc"
if [ -x "$TC_GCC" ] && "$TC_GCC" --version >/tmp/tcgcc.log 2>&1; then
    echo "  PASS control: $TC_GCC runs ($(sed -n 1p /tmp/tcgcc.log))"
else
    echo "  FAIL control: $TC_GCC missing or does not run"
    sed "s/^/       /" /tmp/tcgcc.log 2>/dev/null; rc=1
fi

# 2. AXL_TOOLCHAIN unset -- what auto ITSELF resolves, not a pinned variant.
if [ -n "${AXL_TOOLCHAIN:-}" ]; then
    echo "  FAIL control: AXL_TOOLCHAIN is set (\"$AXL_TOOLCHAIN\") -- auto is not what runs"
    rc=1
else
    echo "  PASS control: AXL_TOOLCHAIN is unset -- auto is what runs"
fi

cd /tmp
curl -fsSLO file:///rel/install.sh || { echo "FAIL: could not fetch install.sh"; exit 1; }
if ! sh install.sh --yes --base-url file:///rel > /tmp/install.log 2>&1; then
    echo "FAIL: install.sh exited nonzero"; tail -8 /tmp/install.log; exit 1
fi
echo "  PASS install.sh completes with both compilers present"
export PATH="$HOME/.local/bin:$PATH"

want "axl --version" "axl $V" "$(axl --version 2>&1 | head -1)"

# build_axl <label> <src> <out> -- axl-cc --verbose echoes every compile,
# link and objcopy command it runs with the FULL path of the binary (run_cmd
# in scripts/axl-cc), so the log names the exact toolchain that produced the
# image. That is the discriminator for point 4: both compilers emit a valid
# PE32+ x86-64 file, so the file existing proves nothing about WHICH one built
# it -- the invocation lines pin the answer to the mounted absolute path
# rather than the "axl" word a stale reader could satisfy by other means.
build_axl() {
    axl-cc --verbose "$2" -o "$3" > "$4" 2>&1
    _brc=$?
    if [ "$_brc" -eq 0 ] && [ -s "$3" ]; then
        _off=$(od -An -tu4 -j60 -N4 "$3" | tr -d " ")
        _m=$(od -An -tx2 -j $((_off + 4)) -N2 "$3" | tr -d " ")
        if [ "$_m" = "8664" ]; then
            echo "  PASS $1 ($(wc -c < "$3" | tr -d " ") bytes, PE32+ x86-64)"
        else
            echo "  FAIL $1: not a PE32+ x86-64 image (machine word $_m)"; rc=1
        fi
    else
        echo "  FAIL $1: axl-cc exited $_brc or produced no file"
        sed "s/^/       /" "$4" | tail -8; rc=1
    fi
    if grep -qF "+ $TC_GCC " "$4"; then
        echo "  PASS $1: compiled by $TC_GCC, not the Ubuntu gcc"
    else
        echo "  FAIL $1: compile line does not name $TC_GCC"; rc=1
    fi
}

build_axl "hello.c" /ex/hello.c /tmp/hello.efi /tmp/hello.verbose.log

# 3. auto resolves to axl on the MECHANISM, not just the result: the banner
# must read "axl (auto)", never the bare word -- a bare match would also
# pass if something had pinned the variant, which is the hole this arm
# exists to close (test-host-toolchain-qemu.sh:324 makes the same call for
# the same reason).
if grep -qF "toolchain=axl (auto)" /tmp/hello.verbose.log; then
    echo "  PASS auto resolves to axl (auto), not a pinned axl"
else
    echo "  FAIL auto did not resolve to \"axl (auto)\""
    grep -m1 "^\[axl-cc\] " /tmp/hello.verbose.log | sed "s/^/       /"; rc=1
fi

# The bare-metal LINKER and OBJCOPY ran too, not just the compiler -- all
# three tools differ between the two toolchains, and a link/convert step
# quietly falling back to the host binutils would be invisible from the
# compile line alone.
if grep -qF "+ $X64_TC/bin/x86_64-elf-ld " /tmp/hello.verbose.log; then
    echo "  PASS hello.efi linked by the bare-metal ld"
else
    echo "  FAIL hello.efi link line does not name the bare-metal ld"; rc=1
fi
if grep -qF "+ $X64_TC/bin/x86_64-elf-objcopy " /tmp/hello.verbose.log; then
    echo "  PASS hello.efi converted by the bare-metal objcopy"
else
    echo "  FAIL hello.efi objcopy line does not name the bare-metal objcopy"; rc=1
fi

# 5. http-server.c is NOT optional, for the same reason it is not optional in
# the host-gcc arm above: --gc-sections drops the vendored code (libvterm,
# lzma, mbedTLS) that a trivial source never references, so hello.c alone
# would pass even against a C library missing pieces http-server.c needs.
build_axl "http-server.c" /ex/http-server.c /tmp/http-server.efi /tmp/http.verbose.log

exit $rc
'

# Mounts the toolchain like run_distro, AND installs gcc/binutils like
# run_distro_hostgcc -- it is a sibling to both rather than a flag bolted onto
# either, because its assertion script is genuinely different content (the
# controls assert PRESENCE where CONSUMER_HOSTGCC asserts absence, and it
# reads --verbose invocation lines neither existing script needs), and both
# of those files are deliberately kept conditional-free for the one thing
# they already check. Bootstrap list matches run_distro_hostgcc's exactly
# (gcc + binutils are the subject, curl is install.sh's own need_cmd).
run_distro_both() {  # run_distro_both <label> <image> <bootstrap-cmd>
    local label="$1" image="$2" bootstrap="$3"
    local log="$WORK/${label}.log"
    podman run --rm \
        -v "$REL:/rel:ro" \
        -v "$X64_TC:$X64_TC:ro" \
        -v "$PROJECT_DIR/sdk/examples:/ex:ro" \
        -e "V=$VERSION" \
        -e "X64_TC=$X64_TC" \
        "$image" sh -c "$bootstrap
$CONSUMER_BOTH" > "$log" 2>&1
    local rc=$?
    sed 's/^/    /' "$log"
    # Same reasoning as run_distro_hostgcc's own count: a zero exit is not
    # enough on its own, because a container that never reached the script
    # would also exit 0. 12 as of 2026-09-03 (3 control lines, install.sh,
    # --version, 2x[image+compiled-by] for hello.c, the auto-mechanism line,
    # ld, objcopy, and 2x[image+compiled-by] for http-server.c).
    local npass
    npass=$(grep -c "^  PASS " "$log" || true)
    if [[ "$rc" -eq 0 && "$npass" -lt 12 ]]; then
        echo "    the arm printed only $npass PASS lines, fewer than the 12 it promises"
        rc=1
    fi
    check "$rc" "$label: auto resolves to axl when both compilers are present, and builds with it"
}

run_distro_both "ubuntu(both)" docker.io/library/ubuntu:24.04 \
    '{ apt-get update -qq && apt-get install -y -qq gcc binutils curl; } > /tmp/apt.log 2>&1 \
        || { echo "FAIL: could not apt-get gcc/binutils/curl"; tail -8 /tmp/apt.log; exit 1; }'

test_host_summary "consumer-install"
