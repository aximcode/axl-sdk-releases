# shellcheck shell=sh
# container-pkg.sh -- install distribution packages INSIDE a throwaway
# container, bounded, and fail with a named cause.
#
# POSIX sh, and NOT sourced on the host: a caller prepends this file's text to
# the script it hands `podman run ... sh -c`, so it runs under dash (Debian),
# bash (Fedora) or whatever the image's /bin/sh is. It defines one entry
# point, `pkg_install <package>...`, which picks apt-get or dnf.
#
# WHY IT EXISTS (2026-11-09). archive.ubuntu.com went away for ~30 minutes and
# then answered slowly. The in-container installs of the consumer tests sent
# the package manager's output to /dev/null, so a dead mirror surfaced as a
# 756 s CTest kill with no line naming the mirror -- or, because a plain
# `apt-get update` exits 0 when every index fails to download, as "Unable to
# locate package curl" one step later. Three properties fix that:
#
#   - EACH FETCH IS BOUNDED (apt Acquire::Retries/Timeout, dnf
#     timeout/retries). apt's own defaults retry for a long time on a mirror
#     that does not answer.
#   - THE WHOLE CALL IS BOUNDED BY A WALL CLOCK (`timeout`, AXL_PKG_WALL
#     seconds, default 60). The per-fetch bounds alone do NOT bound the
#     total: apt's Timeout is per read, so a mirror that trickles bytes never
#     trips it, and Retries multiplies across every package. ci.yml has
#     carried the same per-fetch settings since 2026-05 and its clang-tidy
#     install still sat silent for 19 minutes.
#   - `apt-get update --error-on=any`, so an index that failed to download is
#     a failure HERE, named, rather than a missing package later.
#
# On failure it prints, and then exits the container's shell with status 1:
#
#   AXL-PKG-FAIL: <step> failed (exit N | timed out after Ns)
#     likely cause: the package mirror or the network, not AXL (mirrors: ...)
#     last lines of /tmp/axl-pkg.log:
#       ...
#
# The first line is a stable marker: test-consumer-install.sh greps it to stop
# running further arms once a mirror has failed. An image with neither
# package manager prints AXL-IMAGE-FAIL instead -- a broken image, not an
# outage, so it does not stop the other arms.
#
# WHY 60 s. A healthy call takes well under 20 s on every image these tests
# use, and every caller is a test inside CI's 240 s per-test limit: two calls
# that each stall to the wall (+10 s KILL grace) must still leave the test
# room to print the marker itself, rather than be killed unnamed by the
# runner. What this cannot bound is a mirror that trickles and SUCCEEDS just
# inside the wall on call after call; that run can still reach the runner's
# limit, which then names itself (common-test.sh's KILLED line) and leaves no
# container behind (lib/podman-run.sh). It stays a FAILURE, never a
# skip -- a suite that certifies releases must not certify an install nobody
# ran.

AXL_PKG_WALL=${AXL_PKG_WALL:-60}
AXL_PKG_LOG=/tmp/axl-pkg.log
export DEBIAN_FRONTEND=noninteractive

# The mirrors this image is configured to use, for the failure message. deb822
# `URIs:` lines (Debian/Ubuntu today) and one-line `deb URL` sources both
# count; for dnf, the repo hosts.
_axl_pkg_mirrors() {
    if [ -d /etc/apt ]; then
        cat /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null \
            | sed -nE 's/^URIs:[[:space:]]*//p; s/^deb[[:space:]]+(\[[^]]*\][[:space:]]*)?([^[:space:]]+).*/\2/p' \
            | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ' | sed 's/ $//'
    elif [ -d /etc/yum.repos.d ]; then
        sed -nE 's#^(metalink|mirrorlist|baseurl)=(https?://[^/]+).*#\2#p' \
            /etc/yum.repos.d/*.repo 2>/dev/null | sort -u | tr '\n' ' ' | sed 's/ $//'
    fi
}

_axl_pkg_fail() {  # _axl_pkg_fail <exit-status> <step>
    # 124 is timeout's TERM, 137 its KILL after the grace period.
    if [ "$1" -eq 124 ] || [ "$1" -eq 137 ]; then
        _axl_why="timed out after ${AXL_PKG_WALL}s"
    else
        _axl_why="exit $1"
    fi
    echo "AXL-PKG-FAIL: $2 failed ($_axl_why)"
    echo "  likely cause: the package mirror or the network, not AXL (mirrors: $(_axl_pkg_mirrors))"
    echo "  last lines of $AXL_PKG_LOG:"
    tail -8 "$AXL_PKG_LOG" | sed 's/^/    /'
    exit 1
}

_axl_pkg_run() {  # _axl_pkg_run <step> <command>...
    _axl_step=$1; shift
    echo "+ $*" >> "$AXL_PKG_LOG"
    timeout -k 10 "$AXL_PKG_WALL" "$@" >> "$AXL_PKG_LOG" 2>&1 || _axl_pkg_fail "$?" "$_axl_step"
}

pkg_install() {  # pkg_install <package>...
    if command -v apt-get >/dev/null 2>&1; then
        _axl_apt_o="-o Acquire::Retries=3 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15"
        # Once per container: the indexes do not go stale between two calls.
        if [ -z "${_axl_apt_updated:-}" ]; then
            # shellcheck disable=SC2086
            _axl_pkg_run "apt-get update" apt-get -q $_axl_apt_o --error-on=any update
            _axl_apt_updated=1
        fi
        # shellcheck disable=SC2086
        _axl_pkg_run "apt-get install $*" apt-get -q $_axl_apt_o install -y "$@"
    elif command -v dnf >/dev/null 2>&1; then
        _axl_pkg_run "dnf install $*" dnf -q --setopt=timeout=15 --setopt=retries=3 install -y "$@"
    else
        echo "AXL-IMAGE-FAIL: no apt-get or dnf on this image"
        exit 1
    fi
}
