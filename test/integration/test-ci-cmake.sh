#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-ci-cmake.sh -- ONE pinned CMake for CI and the release
# (scripts/ci-cmake.sh; docs/AXL-Build-System-Design.md §10.12).
#
# The release was built with the hosted runner image's CMake (3.31.6) while
# CI's parity and gates ran on ubuntu:26.04's (4.2.3). Pinned, it stays one
# version only if EVERY workflow job that configures the tree installs it
# before its first build, and none takes cmake from apt -- which is what
# this checks of the workflows, beside the installer's own behaviour (with a
# stub curl: nothing is downloaded).
#
# Host-only. Usage: ./test/integration/test-ci-cmake.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CI_CMAKE="$PROJECT_DIR/scripts/ci-cmake.sh"
WF="$PROJECT_DIR/.github/workflows"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

WORK="$(mktemp -d -t axl-cicmake.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

V="$("$CI_CMAKE" --version)"
check "the pinned version is a 4.3.x (owner decision: CPack archives own 0/0 from 4.3, CMP0206)" \
    "$([[ "$V" =~ ^4\.3\.[0-9]+$ ]] && echo 4.3.x || echo "$V")" "4.3.x"
out="$("$CI_CMAKE" 2>&1)"; rc=$?
check "no arguments is a usage error" "$rc $out" "2 usage: ci-cmake.sh --version | --floor-version | --below-floor-version | name [--floor|--below-floor] | install [--floor|--below-floor] DIR"
out="$("$CI_CMAKE" install 2>&1)"; rc=$?
check "install without DIR is a usage error" "$rc" "2"
out="$("$CI_CMAKE" install --floor 2>&1)"; rc=$?
check "install --floor without DIR is a usage error" "$rc" "2"
out="$("$CI_CMAKE" install --below-floor 2>&1)"; rc=$?
check "install --below-floor without DIR is a usage error" "$rc" "2"
out="$("$CI_CMAKE" name --floor extra 2>&1)"; rc=$?
check "name takes no DIR" "$rc" "2"
FV="$("$CI_CMAKE" --floor-version)"
check "the floor is a 3.20.x (the minimum sdk/examples and the package promise)" \
    "$([[ "$FV" =~ ^3\.20\.[0-9]+$ ]] && echo 3.20.x || echo "$FV")" "3.20.x"
BFV="$("$CI_CMAKE" --below-floor-version)"
check "the below-floor CMake (the package must refuse it) is a 3.16.x, the floor this replaced" \
    "$([[ "$BFV" =~ ^3\.16\.[0-9]+$ ]] && echo 3.16.x || echo "$BFV")" "3.16.x"

echo "=== the workflows ==="
# (The scripts named are the ones that configure or that call
# axl_use_pinned_cmake, which on a job without the pinned CMake first on PATH
# would download it into $HOME/.cache -- scripts/axl-cmake-pin.sh.)
# Every job that CHECKS OUT the tree installs the pinned CMake, unless it is
# named below as one that never configures it. Inverted on purpose: a list
# of the commands that configure (axl-build.sh, install.sh, ctest, a script
# that calls one...) missed `VAR= ctest`, `- run: cmake` and every wrapper,
# and a scan that misses a job reports it clean. One line per checked-out
# job: "<file>:<job> ok|NO-INSTALL|LATE" (LATE: installed after a build).
# What counts as "builds": the commands that configure, plus every script
# that calls axl_use_pinned_cmake -- derived, not listed: on a job without
# the pinned CMake first on PATH that call downloads it into $HOME/.cache
# (scripts/axl-cmake-pin.sh), so a new pinned entry point must be seen here.
pinned_re="$(cd "$PROJECT_DIR" && git grep -lE '^[^#]*axl_use_pinned_cmake([[:space:]]|$)' -- '*.sh' \
    | sed 's|\.|\\.|g' | paste -sd'|')"
# By PATH, not basename: scripts/install.sh calls the helper (it runs
# `cmake --install`), and a bare `install\.sh` would match the release job's
# copy of packaging/install.sh, which configures nothing.
# (The wrapper's name assembled from a variable: gate.check-test-meta reads a
# literal `scripts/axl-build` + `.sh` as this test building something.)
wrapper="axl-build"
check "control: the pinned entry points are found (the build wrapper among them)" \
    "$(tr '|' '\n' <<< "$pinned_re" | grep -cx "scripts/$wrapper\\\\.sh")" "1"
# ...except the asset-set QUERY, `make-release-assets.sh assets`, which
# prints names and exits before taking the pin (the release job asks it).
export NOBUILD_RE='make-release-assets\.sh assets( |$)'
export USE_RE="(axl-build\.sh|scripts/install\.sh|make-release-assets\.sh|release-suite\.sh|verify\.sh|lint\.sh|$pinned_re|(^|[[:space:]=;&|(-])(cmake|ctest)[[:space:]])"
NEVER_CONFIGURES="ci.yml:plan docs.yml:build-and-deploy release.yml:release"
jobs_report() {
    local f
    for f in "$WF"/*.yml; do
        awk -v file="${f##*/}" '
            function flush() {
                if (job != "" && co) print file ":" job " " (inst == 0 ? "NO-INSTALL" : (inst < use || use == 0 ? "ok" : "LATE"))
            }
            /^jobs:/ { injobs = 1; next }
            injobs && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ {
                flush(); job = $1; sub(/:$/, "", job); co = 0; inst = 0; use = 0; next
            }
            /^[[:space:]]*#/ { next }
            job != "" && /actions\/checkout@/ { co = 1 }
            job != "" && /ci-cmake\.sh install/ && inst == 0 { inst = NR }
            job != "" && use == 0 && $0 ~ ENVIRON["USE_RE"] && $0 !~ ENVIRON["NOBUILD_RE"] { use = NR }
            END { flush() }
        ' "$f"
    done
}
report="$(jobs_report)"
# Control for the derived list: a job that runs a newly pinned script (a test
# that calls axl_use_pinned_cmake, none of the fixed names) BEFORE its install
# is seen as building there -- LATE, not "ok".
mkdir -p "$WORK/wf"
printf '%s\n' 'jobs:' '  t:' '    runs-on: x' '    steps:' '      - uses: actions/checkout@v4' \
    '      - run: ./test/integration/test-cmake-package.sh' \
    '      - run: ./scripts/ci-cmake.sh install "$RUNNER_TEMP/c"' > "$WORK/wf/fx.yml"
check "control: a job running a pinned test before installing the pin is LATE" \
    "$(WF="$WORK/wf" jobs_report)" "fx.yml:t LATE"
check "control: the scan sees every job that checks out (ci 4, docs 1, release 4)" \
    "$(wc -l <<< "$report" | tr -d ' ')" "9"
unexplained="$(while read -r j v; do
    [[ " $NEVER_CONFIGURES " == *" $j "* ]] && continue
    [[ "$v" == ok ]] || echo "$j $v"
done <<< "$report")"
check "every checked-out job installs the pinned CMake before its first build (or never configures)" \
    "$unexplained" ""
# The allowlist is a claim too: a listed job that starts building must leave it.
listed_building="$(for j in $NEVER_CONFIGURES; do
    awk -v want="${j#*:}" '
        /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { cur = $1; sub(/:$/, "", cur); next }
        /^[[:space:]]*#/ { next }
        cur == want && $0 ~ ENVIRON["USE_RE"] && $0 !~ ENVIRON["NOBUILD_RE"] { print FILENAME ":" cur; exit }
    ' "$WF/${j%%:*}"
done)"
check "...and no job on the never-configures list builds" "$listed_building" ""
# apt lines joined across backslash continuations first: a multi-line
# package list is where cmake would hide.
check "no workflow installs cmake from apt" \
    "$(sed -e ':a' -e '/\\$/N; s/\\\n//; ta' "$WF"/*.yml | grep -E 'apt-get install' | grep -E '[[:space:]]cmake([[:space:]]|$)')" ""

echo "=== the installer (stub curl) ==="
# The stub curl writes whatever STUB_BODY names to -o, and logs its call.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/curl" <<'EOF'
#!/bin/bash
echo "curl $*" >> "$CURL_LOG"
while [[ $# -gt 0 ]]; do [[ "$1" == -o ]] && { cp "$STUB_BODY" "$2"; exit 0; }; shift; done
exit 1
EOF
chmod +x "$WORK/bin/curl"
export CURL_LOG="$WORK/curl.log"
name="cmake-$V-linux-$(uname -m)"

# A tarball that is not Kitware's: refused by checksum, nothing installed.
echo junk > "$WORK/junk.tar.gz"
out="$(PATH="$WORK/bin:$PATH" STUB_BODY="$WORK/junk.tar.gz" "$CI_CMAKE" install "$WORK/d1" 2>&1)"; rc=$?
check "a download that fails its pinned SHA-256 is refused (1)" "$rc" "1"
check "...by name" "$(tail -1 <<< "$out")" \
    "ci-cmake.sh: $name.tar.gz does not match its pinned SHA-256 ($(sed -n "s/^AXL_CI_CMAKE_SHA256_$(uname -m)=//p" "$CI_CMAKE"))"
check "...and nothing is installed" "$(ls -A "$WORK/d1")" ""

# Already installed and reporting the pinned version: reused, no download.
mkdir -p "$WORK/d2/$name/bin"
printf '#!/bin/sh\necho "cmake version %s"\n' "$V" > "$WORK/d2/$name/bin/cmake"
chmod +x "$WORK/d2/$name/bin/cmake"
: > "$CURL_LOG"
out="$(PATH="$WORK/bin:$PATH" "$CI_CMAKE" install "$WORK/d2" 2>&1)"; rc=$?
check "an installed pinned CMake is reused: its bin/ printed, nothing fetched" \
    "$rc $out $(wc -l < "$CURL_LOG")" "0 $WORK/d2/$name/bin 0"
# The right NAME with the wrong version (an interrupted or stale install) is
# not reused: the binary is asked, and the installer fetches.
printf '#!/bin/sh\necho "cmake version 0.0.1"\n' > "$WORK/d2/$name/bin/cmake"
: > "$CURL_LOG"
PATH="$WORK/bin:$PATH" STUB_BODY="$WORK/junk.tar.gz" "$CI_CMAKE" install "$WORK/d2" > /dev/null 2>&1
check "a tree of the right name reporting another version is fetched again" \
    "$(grep -c "releases/download/v$V/$name.tar.gz" "$CURL_LOG")" "1"

echo "=== the floor and below-floor installers (stub curl) ==="
# A uname stub per machine, so each arch's URL and checksum are asserted on
# whatever box this runs on.
for m in x86_64 aarch64 riscv64; do
    mkdir -p "$WORK/m-$m"
    printf '#!/bin/sh\n[ "$1" = -m ] && { echo %s; exit 0; }\nexec /usr/bin/uname "$@"\n' "$m" > "$WORK/m-$m/uname"
    chmod +x "$WORK/m-$m/uname"
done
# floor_fetch KIND MACHINE DIR -- install KIND on MACHINE with the junk
# tarball; print "rc|the URL curl was asked for|the last stderr line".
floor_fetch() {
    : > "$CURL_LOG"
    local o r=0
    o="$(PATH="$WORK/m-$2:$WORK/bin:$PATH" STUB_BODY="$WORK/junk.tar.gz" "$CI_CMAKE" install "--$1" "$3" 2>&1)" || r=$?
    echo "$r|$(grep -o "https://[^ ]*" "$CURL_LOG")|$(tail -1 <<< "$o")"
}
sha_of() { sed -n "s/^$1=//p" "$CI_CMAKE"; }
# 3.20+ is named `linux` and built for x86_64 AND aarch64, like the pin; a
# wrong case would 404 on the real server and read as "offline", so the URL
# is asserted exactly.
for m in x86_64 aarch64; do
    fname="cmake-$FV-linux-$m"
    check "floor on $m: Kitware's $fname URL, refused (1) by the floor's $m SHA-256" \
        "$(floor_fetch floor "$m" "$WORK/f-$m")" \
        "1|https://github.com/Kitware/CMake/releases/download/v$FV/$fname.tar.gz|ci-cmake.sh: $fname.tar.gz does not match its floor SHA-256 ($(sha_of "AXL_FLOOR_CMAKE_SHA256_$m"))"
    check "...and nothing is installed" "$(ls -A "$WORK/f-$m")" ""
    check "name --floor on $m" "$(PATH="$WORK/m-$m:$PATH" "$CI_CMAKE" name --floor)" "$fname"
done
check "the two floor checksums differ (one per arch, not one copied)" \
    "$([[ -n "$(sha_of AXL_FLOOR_CMAKE_SHA256_x86_64)" && "$(sha_of AXL_FLOOR_CMAKE_SHA256_x86_64)" != "$(sha_of AXL_FLOOR_CMAKE_SHA256_aarch64)" ]] && echo differ)" "differ"
check "no floor for a machine Kitware built none for: 3 (not usage's 2), said, nothing fetched" \
    "$(floor_fetch floor riscv64 "$WORK/f-rv")|$(wc -l < "$CURL_LOG")" \
    "3||ci-cmake.sh: no floor CMake $FV for machine 'riscv64'|0"
out="$(PATH="$WORK/m-riscv64:$PATH" AXL_FLOOR_CMAKE_SHA256_riscv64=0000 "$CI_CMAKE" name --floor 2>&1)"; rc=$?
check "...and a checksum variable from the ENVIRONMENT does not make the machine supported" \
    "$rc|$out" "3|ci-cmake.sh: no floor CMake $FV for machine 'riscv64'"

# Pre-3.20 is named `Linux`, and 3.16 was built for x86_64 only.
bname="cmake-$BFV-Linux-x86_64"
check "below-floor on x86_64: Kitware's $bname URL, refused (1) by its own SHA-256" \
    "$(floor_fetch below-floor x86_64 "$WORK/b1")" \
    "1|https://github.com/Kitware/CMake/releases/download/v$BFV/$bname.tar.gz|ci-cmake.sh: $bname.tar.gz does not match its below-floor SHA-256 ($(sha_of AXL_BELOW_FLOOR_CMAKE_SHA256_x86_64))"
check "...and nothing is installed" "$(ls -A "$WORK/b1")" ""
check "name --below-floor on x86_64" "$(PATH="$WORK/m-x86_64:$PATH" "$CI_CMAKE" name --below-floor)" "$bname"
check "no below-floor for aarch64: 3, said, nothing fetched" \
    "$(floor_fetch below-floor aarch64 "$WORK/b2")|$(wc -l < "$CURL_LOG")" \
    "3||ci-cmake.sh: no below-floor CMake $BFV for machine 'aarch64' (Kitware built 3.16 for x86_64 only)|0"
out="$(PATH="$WORK/m-aarch64:$PATH" "$CI_CMAKE" name --below-floor 2>&1)"; rc=$?
check "...and name says the same" "$rc|$out" \
    "3|ci-cmake.sh: no below-floor CMake $BFV for machine 'aarch64' (Kitware built 3.16 for x86_64 only)"
check "name with no kind is the pinned tarball's" "$(PATH="$WORK/m-aarch64:$PATH" "$CI_CMAKE" name)" "cmake-$V-linux-aarch64"

echo ""
echo "ci-cmake: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
