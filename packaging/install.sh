#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
#
# install.sh — install the AXL SDK, its host tools, and optionally the
# bare-metal cross toolchain.
#
#   curl -fsSLO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/install.sh
#   sh install.sh --help
#
# Download it and run it; the piped form works for the default case but the
# install is options-driven and `sh -s --` makes options ugly. See
# AXL-Distribution-Design.md §17-§18.
#
# STRUCTURE IS LOad-BEARING. Everything below is a function and nothing runs
# until `main "$@"` on the last line. That is the guard against a truncated
# `curl | sh`: a connection dropped mid-download leaves sh a prefix of this
# file, and a prefix of a file that only DEFINES functions does nothing. The
# linear first draft of this script ran `rm -rf "$PREFIX_ROOT/$DIR"` at top
# level, so a cut one line later deleted an existing install and did not
# replace it. Copied from rustup-init.sh, which ends the same way.
#
# POSIX sh, not bash: this is the one AXL script that runs before anything of
# ours is installed, on a machine we know nothing about.

set -eu

REPO="aximcode/axl-sdk-releases"
BASE="https://github.com/${REPO}/releases"

# AXL_INSECURE_FETCH=1 -- skip TLS verification on the downloads below.
#
# FOR A CORPORATE MITM PROXY. Dell (and others) intercept HTTPS org-wide with a
# CA a fresh machine does not trust, so these fetches die at TLS on a host that
# is otherwise fine. Requiring every such user to install a corporate CA chain
# is per-machine setup we can avoid.
#
# WHAT IT ACTUALLY COSTS, stated precisely because the honest answer differs by
# script. `scripts/install-toolchain.sh` verifies against a SHA256 that SHIPS IN
# THE SDK (axl-toolchains.conf) -- pre-shared, out-of-band, and no attacker can
# forge an artifact to match it, so `-k` there costs nothing at all.
#
# HERE IT IS WEAKER. This script verifies against SHA256SUMS, which it fetches
# from the SAME base URL as the assets -- so with TLS off, whoever can substitute
# an asset can substitute the sums that vouch for it. The guarantee drops from
# AUTHENTICATED to CORRUPTION-RESISTANT. It is still worth having (a truncated
# or mangled download is caught), and it is genuinely sound for a caller that
# pinned the hashes out-of-band -- which the flagship consumer does. But it is
# not "the hash is the trust anchor", and main() says so out loud when the flag
# is on rather than letting the weaker guarantee read as the stronger one.
INSECURE=""

die()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
info() { printf '  %s\n' "$*"; }
warn() { printf 'install.sh: %s\n' "$*" >&2; }

# rustup's discipline: name the missing tool, and never let a failed command
# be mistaken for a completed step.
need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "need '$1' on PATH"
}
ensure() {
    "$@" || die "command failed: $*"
}

usage() {
    cat <<EOF
usage: sh install.sh [options]

  --version X.Y.Z     install this version (default: the latest release)
  --prefix DIR        install root (default: \${XDG_DATA_HOME:-\$HOME/.local/share})
  --bin-dir DIR       where to link the commands (default: \${XDG_BIN_HOME:-\$HOME/.local/bin})
  --host-tools        install only the host tools (run-qemu and friends, no compiler)
  --toolchain ARCH    also install the cross toolchain: x64 | aa64 | all
  --uninstall         remove an installed version and its links
  --use VERSION       make an already-installed version current; download it
                      first only if it is not present
  --force             reinstall even if that version is already present
  --prune             after installing, offer to remove superseded versions
  --base-url URL      fetch from here instead of GitHub (mirror, or a local dir)

Environment:
  AXL_BIN_DIR         where to put the command links, when --bin-dir is not
                      given. Recorded in the prefix, so `axl use` / `axl
                      uninstall` later relink and unlink the same directory
                      rather than guessing it again.
  AXL_INSECURE_FETCH=1  skip TLS verification when downloading. For a host
                      behind a corporate MITM proxy whose CA is not installed.
                      Downloads are still checked against SHA256SUMS -- but
                      SHA256SUMS is fetched over the same connection, so this
                      protects against corruption, NOT against a substitution
                      by whoever is intercepting. Sound if you pinned the
                      hashes yourself, out of band.
  -y, --yes           do not prompt
  -h, --help          this

Everything installs under one versioned directory and is removed by deleting
it. Nothing is written outside the prefix except the toolchain, which lives in
/opt because a compiler is shared between SDK versions.
EOF
}

parse_args() {
    VERSION=""; PREFIX_ROOT=""; BIN_DIR=""; COMPONENT="sdk"
    TOOLCHAIN=""; DO_UNINSTALL=0; ASSUME_YES=0; BASE_URL=""; DO_PRUNE=0
    FORCE=0; USE_VERSION=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --version)     VERSION="${2:?--version needs a value}"; shift 2 ;;
            --version=*)   VERSION="${1#*=}"; shift ;;
            --prefix)      PREFIX_ROOT="${2:?--prefix needs a value}"; shift 2 ;;
            --prefix=*)    PREFIX_ROOT="${1#*=}"; shift ;;
            --bin-dir)     BIN_DIR="${2:?--bin-dir needs a value}"; shift 2 ;;
            --bin-dir=*)   BIN_DIR="${1#*=}"; shift ;;
            --host-tools)  COMPONENT="host-tools"; shift ;;
            --toolchain)   TOOLCHAIN="${2:?--toolchain needs a value}"; shift 2 ;;
            --toolchain=*) TOOLCHAIN="${1#*=}"; shift ;;
            --uninstall)   DO_UNINSTALL=1; shift ;;
            --force)       FORCE=1; shift ;;
            --use)         USE_VERSION="${2:?--use needs a version}"; shift 2 ;;
            --use=*)       USE_VERSION="${1#*=}"; shift ;;
            --prune)       DO_PRUNE=1; shift ;;
            --base-url)    BASE_URL="${2:?--base-url needs a value}"; shift 2 ;;
            --base-url=*)  BASE_URL="${1#*=}"; shift ;;
            -y|--yes)      ASSUME_YES=1; shift ;;
            -h|--help)     usage; exit 0 ;;
            *)             die "unknown option '$1' (try --help)" ;;
        esac
    done
    case "$TOOLCHAIN" in ""|x64|aa64|all) ;; *) die "--toolchain wants x64, aa64 or all" ;; esac
    PREFIX_ROOT="${PREFIX_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}}"
    # AXL_BIN_DIR is read HERE too, not only by `axl`. It used to be honoured
    # by the dispatcher alone, so `AXL_BIN_DIR=/x sh install.sh` installed into
    # ~/.local/bin while `AXL_BIN_DIR=/x axl use ...` relinked into /x -- the
    # same split-brain the recorded bin dir below exists to remove.
    BIN_DIR="${BIN_DIR:-${AXL_BIN_DIR:-${XDG_BIN_HOME:-$HOME/.local/bin}}}"
    case "$COMPONENT" in
        sdk)        STEM="axl-sdk";            CURRENT="axl-sdk";            LABEL="AXL SDK" ;;
        host-tools) STEM="axl-sdk-host-tools"; CURRENT="axl-sdk-host-tools"; LABEL="AXL host tools" ;;
    esac
}

# A package install and a prefix install both put `axl` on PATH, and the
# package wins (/usr/bin before ~/.local/bin on most distros). §17.3.
check_package_install() {
    _pkg=""
    if command -v dpkg >/dev/null 2>&1 && dpkg -s axl-sdk >/dev/null 2>&1; then
        _pkg="apt remove axl-sdk"
    elif command -v rpm >/dev/null 2>&1 && rpm -q axl-sdk >/dev/null 2>&1; then
        _pkg="dnf remove axl-sdk"
    fi
    [ -n "$_pkg" ] || return 0
    warn "axl-sdk is also installed as a system package."
    warn "  /usr/bin/axl will win on PATH over anything installed here."
    warn "  Remove it first:  sudo $_pkg"
    [ "$ASSUME_YES" -eq 1 ] || die "re-run with --yes to install anyway"
}

# VERSION becomes a directory-name component ($STEM-$VERSION) that lands
# straight in `rm -rf` (do_uninstall) or a same-filesystem `mv`
# (extract_tree). It is reached from three places -- the fetched VERSION
# asset, --version, and --use -- and all three are untrusted once you count a
# MITM'd fetch or a copy-pasted command. Every version this project has ever
# shipped is bare X.Y.Z (bump-version.sh enforces that going in); release.yml
# additionally anticipates a prerelease suffix (v0.1.3-rc1, v0.0.0-test) for
# tags cut by hand outside cut-release.sh, so that shape is accepted too. The
# literal "axl-sdk-"/"axl-sdk-host-tools-" prefix means this was never a route
# to / or ~ -- but a `/`, `..`, whitespace or a shell metacharacter in VERSION
# still has no business becoming a path component, so all of that is refused
# outright rather than merely checked for non-empty.
validate_version() {
    case "$1" in
        *[!0-9A-Za-z.-]*) die "not a valid version: '$1'" ;;
    esac
    case "$1" in
        *..*) die "not a valid version: '$1'" ;;
    esac
    case "$1" in
        [0-9]*.[0-9]*.[0-9]*) ;;
        *) die "not a valid version: '$1'" ;;
    esac
}

# The prefix is self-contained (§12.3), so removal is rm -rf plus the links.
# Links are RESOLVED rather than guessed, so one pointing elsewhere is safe.
do_uninstall() {
    _target=""
    if [ -n "$VERSION" ]; then
        validate_version "$VERSION"
        _target="$PREFIX_ROOT/$STEM-$VERSION"
    elif [ -L "$PREFIX_ROOT/$CURRENT" ]; then
        _target="$(cd -P "$PREFIX_ROOT/$CURRENT" 2>/dev/null && pwd)" || _target=""
    fi
    [ -n "$_target" ] && [ -d "$_target" ] \
        || die "nothing to uninstall under $PREFIX_ROOT (try --version X.Y.Z)"

    for _l in "$BIN_DIR"/axl "$BIN_DIR"/axl-cc "$BIN_DIR"/axl-c++ \
              "$BIN_DIR"/axl-install-toolchain; do
        [ -L "$_l" ] || continue
        _r="$(readlink -f "$_l" 2>/dev/null || echo "")"
        case "$_r" in "$_target"/*) rm -f "$_l"; info "removed $_l" ;; esac
    done
    if [ -L "$PREFIX_ROOT/$CURRENT" ]; then
        _c="$(cd -P "$PREFIX_ROOT/$CURRENT" 2>/dev/null && pwd)" || _c=""
        [ "$_c" = "$_target" ] && rm -f "$PREFIX_ROOT/$CURRENT"
    fi
    ensure rm -rf "$_target"
    info "removed $_target"
    info "The cross toolchain under /opt is shared and was NOT removed."
}

resolve_version() {
    if [ -n "$VERSION" ]; then
        validate_version "$VERSION"
        return 0
    fi
    _vurl="${BASE_URL:-$BASE/latest/download}/VERSION"
    VERSION="$(curl $INSECURE -fsSL "$_vurl" 2>/dev/null | tr -d ' \t\r\n')" \
        || die "could not resolve the latest version from $_vurl"
    [ -n "$VERSION" ] || die "the VERSION asset at $_vurl was empty"
    validate_version "$VERSION"
}

# Skip the work when the target is already there. `axl update` on a current
# install should cost one small request and change nothing -- re-extracting the
# same tree would churn the symlink and the bin links for no reason.
already_installed() {
    [ "$FORCE" -eq 0 ] || return 1
    _d="$PREFIX_ROOT/$STEM-$VERSION"
    [ "$COMPONENT" = "sdk" ] && _d="$PREFIX_ROOT/axl-sdk-$VERSION"
    [ -d "$_d" ] || return 1
    [ "$(cat "$_d/share/axl/version" 2>/dev/null)" = "$VERSION" ] || return 1
    # Only "installed" if it is also what the links point at.
    _c="$(cd -P "$PREFIX_ROOT/$CURRENT" 2>/dev/null && pwd)" || return 1
    [ "$_c" = "$_d" ]
}

# The names a release MIGHT publish for this component, newest scheme first.
#
# AXL-Distribution-Design.md §14.1a settled one format string --
# `axl-sdk-<component>-<ver>[-<arch>].tar.gz`. Every release before that used
# the second spelling in each list, and the host-tools asset carried NO version
# at all. `axl use <older>` has to keep reaching those: rolling back is the
# whole reason `axl prune` retains current-plus-one, and an installer that only
# knows today's names breaks it for every release already published.
#
# The list is ordered, not guessed at by 404 -- fetch_and_verify picks the
# first name SHA256SUMS actually lists.
asset_candidates() {
    case "$COMPONENT" in
        sdk)
            echo "axl-sdk-linux-$VERSION-x86_64.tar.gz"
            echo "axl-sdk-$VERSION-linux-x86_64.tar.gz"
            ;;
        host-tools)
            echo "axl-sdk-host-tools-$VERSION.tar.gz"
            echo "axl-sdk-host-tools.tar.gz"
            ;;
    esac
}

# The SHA256SUMS line for EXACTLY this name. Compared as a whole field rather
# than matched as a pattern: an asset name is full of `.`, which a regex reads
# as "any character", so `axl-sdk-4.4.0-...` would also match a hypothetical
# `axl-sdk-4x4y0-...`. Exits non-zero when the release does not list the name.
#
# A leading `./` is stripped before comparing. Our own releases never write one
# (`sha256sum -- *`), but a MIRROR that regenerates its checksums does --
# `find . -exec sha256sum {} +` and `sha256sum ./*.tar.gz` both do, and
# `sha256sum -c` accepts them. Refusing meant reporting "this release publishes
# none of: ..." about a directory that plainly held the asset, which sends the
# reader to entirely the wrong question.
sums_line() {  # sums_line <sha256sums-file> <asset>
    awk -v n="$2" '{ f = $NF; sub(/^\.\//, "", f) }
                   f == n { print; found = 1 }
                   END { exit !found }' "$1"
}

# webi's ordering: download, verify, THEN touch the destination. Nothing under
# the prefix is disturbed until the bytes are known good.
fetch_and_verify() {
    DL="${BASE_URL:-$BASE/download/v$VERSION}"
    set_dir_for_version

    WORK="$(mktemp -d)"
    info "$LABEL $VERSION -> $PREFIX_ROOT/$DIR"

    # SHA256SUMS FIRST, and it does two jobs. It is stable-named across every
    # release ever published and lists exactly what that release contains, so
    # it is how the right asset name is CHOSEN rather than inferred from which
    # download 404s -- a 404 and an unreachable mirror are the same failed
    # curl. It also stops 13 MB being spent before we learn there is nothing
    # to check it against.
    curl $INSECURE -fsSL -o "$WORK/SHA256SUMS" "$DL/SHA256SUMS" \
        || die "could not download $DL/SHA256SUMS"

    ASSET=""; _tried=""
    for _c in $(asset_candidates); do
        _tried="$_tried $_c"
        if sums_line "$WORK/SHA256SUMS" "$_c" >/dev/null; then ASSET="$_c"; break; fi
    done
    [ -n "$ASSET" ] || die "release $VERSION publishes none of:$_tried"

    curl $INSECURE -fsSL -o "$WORK/$ASSET" "$DL/$ASSET" || die "could not download $DL/$ASSET"
    sums_line "$WORK/SHA256SUMS" "$ASSET" > "$WORK/want.sha256"
    ( cd "$WORK" && sha256sum -c want.sha256 >/dev/null 2>&1 ) \
        || die "SHA256 mismatch for $ASSET -- refusing to install"
    info "sha256 OK"
}

# Extract into a staging directory INSIDE the prefix root, then rename it into
# place. Two reasons, both load-bearing:
#
#   - Releases before the rename shipped tarbombs (§14.1c measured six
#     top-level entries in host-tools, forty-three in the tools tarball).
#     `tar -C "$PREFIX_ROOT"` scatters those beside the versioned roots that
#     `axl prune` walks, in the user's data directory. Staging contains the
#     blast radius whatever shape the archive turns out to have, and gives the
#     tarbomb the versioned root it should have had.
#   - The staging directory is a SIBLING of the target, so the final move is a
#     same-filesystem rename: the install is there or it is not, never a
#     half-extracted tree that `already_installed` would later call good.
extract_tree() {
    ensure mkdir -p "$PREFIX_ROOT"
    STAGE="$PREFIX_ROOT/.axl-install.$$"
    rm -rf "$STAGE"
    ensure mkdir -p "$STAGE"
    tar xzf "$WORK/$ASSET" -C "$STAGE" || die "could not extract $ASSET"

    # One top-level directory means the archive carries its own versioned root
    # (the shape every asset has since D2): promote it. Anything else is a
    # tarbomb, and $STAGE itself is the root.
    _top="$(ls -A "$STAGE" | head -1)"
    if [ "$(ls -A "$STAGE" | wc -l)" -eq 1 ] && [ -d "$STAGE/$_top" ]; then
        _src="$STAGE/$_top"
    else
        [ -n "$_top" ] || die "$ASSET extracted to nothing"
        _src="$STAGE"
    fi

    rm -rf "$PREFIX_ROOT/$DIR"
    ensure mv "$_src" "$PREFIX_ROOT/$DIR"
    rm -rf "$STAGE"
    STAGE=""
}

# Separate from extraction so `--use` can point the links at a version that is
# ALREADY on disk without touching the network. That is the rollback path, and
# `axl prune` keeps one previous version precisely so it exists.
link_tree() {
    # `current` (§12.2), PER COMPONENT: `axl prune` protects whatever
    # <root>/axl-sdk resolves to, so letting host-tools claim that name would
    # point the SDK's marker at a tree with no compiler in it.
    ensure ln -sfn "$DIR" "$PREFIX_ROOT/$CURRENT"

    ensure mkdir -p "$BIN_DIR"
    # RECORD IT. Which directory the links live in is a fact known only here,
    # at install time -- it is not derivable from the prefix, and `axl use` /
    # `axl uninstall` re-exec this script and have to pass a --bin-dir. They
    # used to recompute it from the environment, so an install placed with an
    # explicit --bin-dir got its links written to a DIFFERENT directory than
    # the ones already on PATH: the rollback path moved the marker while the
    # stale axl-cc kept compiling with the version you had just left, and
    # uninstall cleaned the other directory. Inferring it from $0 instead was
    # worse -- a second symlink (/usr/local/bin/axl -> ~/.local/bin/axl) made
    # it hijack the wrong directory, which is a regression rather than a fix.
    ensure mkdir -p "$PREFIX_ROOT/$DIR/share/axl"
    printf '%s\n' "$BIN_DIR" > "$PREFIX_ROOT/$DIR/share/axl/bin-dir"
    _n=0
    for _b in "$PREFIX_ROOT/$DIR"/bin/*; do
        [ -f "$_b" ] && [ -x "$_b" ] || continue
        case "${_b##*/}" in pe-set-debug) continue ;; esac  # internal, resolved by path
        ensure ln -sfn "$_b" "$BIN_DIR/${_b##*/}"
        _n=$((_n + 1))
    done
    link_manager
    if [ "$_n" -eq 0 ]; then
        # A pre-rename host-tools tarball has a flat scripts/ and no bin/ at
        # all. Saying nothing here reads as a successful install whose commands
        # are merely absent from PATH, which sends the reader to the wrong
        # question entirely.
        warn "no commands to link: $PREFIX_ROOT/$DIR has no bin/"
        warn "  older host-tools archives ship a flat scripts/ directory."
    else
        info "linked $_n command(s) into $BIN_DIR"
    fi
}

# THE MANAGER IS NOT THE MANAGED (§20).
#
# link_tree symlinks every executable in <prefix>/bin, and `axl` is one of
# them -- so switching versions switched the manager too. Roll back to a
# release predating D1, which staged no libexec/axl/install.sh, and `axl
# update` and `axl use` BOTH died with "no installer at ...". The only escape
# was re-fetching install.sh from a web page, which is the dependency D1
# existed to remove -- and `axl prune` retains a previous version precisely so
# that rollback is possible, so the retention policy shipped with the trap.
#
# So `axl` is pinned to the NEWEST installed prefix that carries a staged
# installer, whichever version is current. Written as "re-resolve every time"
# rather than "never downgrade": the latter dangles the moment that newer
# prefix is uninstalled, while this heals on the next install, use or update.
#
# Only `axl` is treated this way. axl-cc, axl-c++ and axl-install-toolchain
# are per-version TOOLS and must follow the active SDK -- that is the whole
# point of switching.
link_manager() {
    # THE MANAGER IS THE HOST-TOOLS COMPONENT (§20 M2). It is preferred over
    # every SDK prefix, and it needs no new root to be safe: `axl use` only
    # ever moves the SDK marker, and `axl prune` protects the CURRENT manager
    # plus the prefix it is running out of. (It does now walk the
    # `^axl-sdk-host-tools-[0-9]` family -- superseded generations otherwise
    # accumulate forever -- so "outside what prune walks" is no longer the
    # reason this is safe; the explicit protection is.)
    #
    # Linked through the `current` marker rather than the versioned directory,
    # so upgrading the manager does not have to relink anything.
    _mgr="$PREFIX_ROOT/axl-sdk-host-tools"
    if [ -x "$_mgr/bin/axl" ] && [ -f "$_mgr/libexec/axl/install.sh" ]; then
        ensure ln -sfn "$_mgr/bin/axl" "$BIN_DIR/axl"
        return 0
    fi

    # No manager installed: fall back to the newest SDK prefix that can still
    # self-update (§20 M1). That is what stops a rollback stranding an install
    # that predates M2.
    _best=""; _best_v=""
    for _p in "$PREFIX_ROOT"/axl-sdk-*; do
        [ -x "$_p/bin/axl" ] || continue
        [ -f "$_p/libexec/axl/install.sh" ] || continue
        _v="${_p##*/axl-sdk-}"
        case "$_v" in *[!0-9.]*) continue ;; esac   # skip host-tools and friends
        if [ -z "$_best_v" ] || [ "$(printf '%s\n%s\n' "$_best_v" "$_v" \
                                     | sort -V | tail -1)" = "$_v" ]; then
            _best="$_p"; _best_v="$_v"
        fi
    done
    # No staged installer anywhere: leave whatever link_tree made. A manager
    # that cannot self-update still beats no `axl` at all.
    [ -n "$_best" ] || return 0
    ensure ln -sfn "$_best/bin/axl" "$BIN_DIR/axl"
    [ "$_best_v" = "$VERSION" ] || \
        info "axl stays at $_best_v (the newest install that can self-update)"
}

set_dir_for_version() {
    DIR="$STEM-$VERSION"
    [ "$COMPONENT" = "sdk" ] && DIR="axl-sdk-$VERSION"
    return 0
}

# --use: activate a version, downloading only if it is not already here.
do_use() {
    VERSION="$USE_VERSION"
    validate_version "$VERSION"
    set_dir_for_version
    if [ -d "$PREFIX_ROOT/$DIR" ]; then
        info "$LABEL $VERSION is already installed -- switching to it"
        link_tree
    else
        info "$LABEL $VERSION is not installed -- fetching it"
        fetch_and_verify
        extract_tree
        link_tree
    fi
    printf '\n  now using %s %s\n' "$LABEL" "$VERSION"
}

# INSTALL THE MANAGER ALONGSIDE THE SDK (§20 M2).
#
# The SDK prefix carries `axl` too, so this is ~0.36 MB of duplication -- and
# it is what buys a manager that does not move when the SDK does. Without it,
# `axl` lives inside a versioned tree that `axl use` repoints and `axl prune`
# can remove, which is the defect §20.1 measured.
#
# Skipped when installing --host-tools (that IS the manager). It used to be
# skipped whenever ANY manager was present -- "re-fetching it on every SDK
# install would make `axl update` slower for no gain" -- and the gain turned
# out to be the whole point: the manager carries `axl` and the staged
# install.sh, so a manager frozen at whatever version first created it never
# receives a fix to either. Measured by test-install-lifecycle.sh: two SDK
# upgrades left `axl --version` reporting the version from the first install.
#
# Now it tracks the version being installed, FORWARD ONLY. The equality test
# below is the no-op path (`already_installed` is main()'s, not this
# function's, and fires before this is reached -- so a plain `install.sh` run
# on an already-current SDK does NOT repair a stale manager; `axl update`,
# which calls --host-tools explicitly, is what does).
ensure_manager() {
    [ "$COMPONENT" = "sdk" ] || return 0
    _m="$PREFIX_ROOT/axl-sdk-host-tools"
    if [ -x "$_m/bin/axl" ] && [ -f "$_m/libexec/axl/install.sh" ]; then
        _mv=""
        [ -r "$_m/share/axl/version" ] && _mv="$(cat "$_m/share/axl/version")"
        # FORWARD ONLY, and `!=` was not that. `do_use` returns before this,
        # so `axl use <older>` was safe -- but `install.sh --version <older>`
        # runs the full install path and reached here, rolling the manager
        # BACK to the pinned version. That is §20's "stranded on an installer
        # too old to fix itself", reachable from the exact command the
        # upgrade notes teach and from any consumer that pins in CI. The
        # lifecycle test never caught it because it only ever installs
        # ascending.
        #
        # An unversioned manager (`_mv` empty -- the legacy
        # axl-sdk-host-tools.tar.gz carries no share/axl/version) is treated
        # as older, so it gets replaced ONCE and then compares equal. Without
        # that it would re-extract on every single SDK install, forever.
        [ "$_mv" = "$VERSION" ] && return 0
        if [ -n "$_mv" ] && [ "$(printf '%s\n%s\n' "$_mv" "$VERSION" \
                                 | sort -V | tail -1)" != "$VERSION" ]; then
            info "manager $_mv is newer than $VERSION -- leaving it alone"
            return 0
        fi
    fi

    # DOES THIS RELEASE EVEN HAVE ONE? Checked against the SHA256SUMS already
    # downloaded for the SDK -- same release, so it is the authority. Without
    # this the manager install would reach fetch_and_verify, which `die`s
    # rather than returning, and a release or mirror carrying no host-tools
    # asset would kill an SDK install that used to work. `if
    # fetch_and_verify` cannot catch that: die() exits the script.
    if [ -f "${WORK:-}/SHA256SUMS" ] \
       && ! sums_line "$WORK/SHA256SUMS" "axl-sdk-host-tools-$VERSION.tar.gz" >/dev/null \
       && ! sums_line "$WORK/SHA256SUMS" "axl-sdk-host-tools.tar.gz" >/dev/null; then
        info "this release publishes no host-tools component -- skipping the manager"
        return 0
    fi

    info "installing the host tools as the manager"
    # Save and restore the SDK's own state: this reuses the same fetch and
    # extract path, and leaving COMPONENT flipped would make every later step
    # -- toolchain, prune, the closing banner -- describe the wrong component.
    _sdk_dir="$DIR"; _sdk_stem="$STEM"; _sdk_cur="$CURRENT"
    _sdk_label="$LABEL"; _sdk_asset="${ASSET:-}"; _sdk_ver="$VERSION"
    COMPONENT="host-tools"; STEM="axl-sdk-host-tools"
    CURRENT="axl-sdk-host-tools"; LABEL="AXL host tools"
    if fetch_and_verify && extract_tree; then
        link_tree
    else
        warn "could not install the host tools; the SDK is installed and usable,"
        warn "  but 'axl update' will fall back to the SDK's own copy."
    fi
    COMPONENT="sdk"; DIR="$_sdk_dir"; STEM="$_sdk_stem"; CURRENT="$_sdk_cur"
    LABEL="$_sdk_label"; ASSET="$_sdk_asset"; VERSION="$_sdk_ver"
    return 0
}

# QEMU is the one thing the packages did that a script cannot: a script cannot
# DECLARE a dependency, so detect and advise, never install silently (§17.1).
advise_qemu() {
    command -v qemu-system-x86_64 >/dev/null 2>&1 && return 0
    warn "QEMU not found -- needed to RUN a .efi, not to build one."
    if   command -v apt    >/dev/null 2>&1; then
        warn "  sudo apt install qemu-system-x86 ovmf virtiofsd mtools dosfstools"
    elif command -v dnf    >/dev/null 2>&1; then
        warn "  sudo dnf install qemu-system-x86 edk2-ovmf virtiofsd mtools dosfstools"
    elif command -v pacman >/dev/null 2>&1; then
        warn "  sudo pacman -S qemu-system-x86 edk2-ovmf virtiofsd mtools dosfstools"
    fi
}

# Four shipped commands are Python -- `axl rsod-decode`, `axl gdb-syms`,
# `axl extract-fv-shell` and `axl emulate`. ADVISED, not required: axl-cc and
# axl-c++ need no interpreter, so a user who only wants to build a .efi must
# not be blocked. But saying nothing is worse: `axl-cc`'s own crash diagnostics
# name `rsod-decode`, and a user following that advice on a minimal image gets
# "python3: command not found" with nothing pointing at the cause. Found by
# test-consumer-install.sh -- neither debian:stable-slim nor fedora:latest
# ships python3, and all four commands failed there while every other check
# passed.
advise_python() {
    command -v python3 >/dev/null 2>&1 && return 0
    warn "python3 not found -- 4 commands need it:"
    warn "  axl rsod-decode, axl gdb-syms, axl extract-fv-shell, axl emulate"
    warn "  (axl-cc and axl-c++ do not; the rest of the SDK works without it.)"
    if   command -v apt    >/dev/null 2>&1; then warn "  sudo apt install python3"
    elif command -v dnf    >/dev/null 2>&1; then warn "  sudo dnf install python3"
    elif command -v pacman >/dev/null 2>&1; then warn "  sudo pacman -S python"
    fi
}

# THE ONE PLACE THE TOOLCHAIN STEP IS ANNOUNCED, and it is here rather than in
# the caller because HERE is where the work actually happens. main() returns at
# already_installed() well before this, so `axl update` announcing a download
# before exec-ing this script promised one on the most common invocation there
# is -- an update on an install that is already current -- and then did nothing.
# The size and the reason belong on the line that fires only when the fetch
# does.
#
# NON-FATAL, and that is a deliberate reversal of the `ensure` this used to be.
# The step runs AFTER link_tree, so by the time it can fail the SDK is already
# installed and linked -- and `ensure`'s `die` then skipped maybe_prune,
# check_path and the closing banner, leaving a user whose SDK DID update with
# nothing but a toolchain error. A network hiccup or a missing sudo read as a
# failed install. The rest of main() now runs, the banner says plainly which
# half did not happen and how to finish it, and main still exits non-zero so CI
# can see it. `axl update` put this on the DEFAULT path, which is what makes the
# distinction matter.
TOOLCHAIN_FAILED=0
maybe_toolchain() {
    [ -n "$TOOLCHAIN" ] || return 0
    _it="$PREFIX_ROOT/$DIR/bin/axl-install-toolchain"
    [ -x "$_it" ] || die "--toolchain needs the SDK component, not --host-tools"
    # CALLER-NEUTRAL WORDING. This function serves BOTH `axl update` (which
    # computed the arch, so the user needs the reason) and a direct
    # `sh install.sh --toolchain aa64` first install (where they asked, and
    # `axl update` prose is simply wrong). The reason that fits both is the
    # fact itself -- the SDK pins the toolchain -- not a sentence about which
    # command is running.
    info "installing the $TOOLCHAIN cross toolchain (uses sudo for /opt;"
    info "  may download ~55-96 MB). The SDK version pins which toolchain"
    info "  axl-cc uses, so this keeps the two in step."
    if ! "$_it" "$TOOLCHAIN"; then
        TOOLCHAIN_FAILED=1
        warn "the $TOOLCHAIN cross toolchain did NOT install (see above)."
    fi
}

# OFFERED, not absorbed. `axl prune` is a command in the SDK -- discoverable
# from `axl --help`, with --dry-run and --keep N. Reimplementing its policy
# here would make it reachable only at install time (§18).
maybe_prune() {
    [ "$DO_PRUNE" -eq 1 ] || return 0
    _p="$PREFIX_ROOT/$DIR/libexec/axl/axl-prune.sh"
    [ -x "$_p" ] || return 0
    info "superseded versions:"
    "$_p" --dry-run || true
    if [ "$ASSUME_YES" -eq 1 ]; then
        "$_p" || true
    else
        info "run 'axl prune' to remove them"
    fi
}

check_path() {
    case ":$PATH:" in
        *":$BIN_DIR:"*) return 0 ;;
    esac
    warn "$BIN_DIR is not on PATH. Add it:"
    warn "  export PATH=\"$BIN_DIR:\$PATH\""
}

# Both temporaries, from one trap. extract_tree's staging directory lives
# inside the prefix root, so a die() between mkdir and mv would otherwise leave
# a `.axl-install.NNN` beside the versioned roots.
cleanup() {
    [ -n "${WORK:-}" ]  && rm -rf "$WORK"
    [ -n "${STAGE:-}" ] && rm -rf "$STAGE"
    return 0
}

main() {
    WORK=""; STAGE=""
    trap cleanup EXIT INT TERM
    parse_args "$@"

    if [ "${AXL_INSECURE_FETCH:-0}" = "1" ]; then
        INSECURE="-k"
        # Said out loud, every time. The doctrine this tree already follows for
        # log levels and the semver guard: make the exceptional case announce
        # itself, so nobody discovers later that a build was fetching without
        # TLS and assumed the hash covered it.
        warn "AXL_INSECURE_FETCH=1 -- TLS verification is OFF for downloads."
        warn "  Assets are still checked against SHA256SUMS, but SHA256SUMS is"
        warn "  fetched over the same connection: this protects against a"
        warn "  corrupted download, not against substitution by whoever is"
        warn "  intercepting. Sound only if you pinned the hashes out of band."
    fi
    need_cmd curl
    need_cmd tar
    need_cmd sha256sum
    # awk chooses the asset out of SHA256SUMS and tr trims the VERSION asset.
    # Neither is exotic, but this list is not decoration: install.sh refuses by
    # NAME when one is absent, and test-host-deps-minimal.sh derives the set a
    # minimal image must install FROM these calls -- so a use that is not
    # declared here is invisible to the only test that would catch it. Without
    # awk every candidate lookup exits 127 and the installer blames a healthy
    # release for publishing nothing it recognises.
    need_cmd awk
    need_cmd tr

    if [ "$DO_UNINSTALL" -eq 1 ]; then
        do_uninstall
        return 0
    fi

    if [ -n "$USE_VERSION" ]; then
        do_use
        return 0
    fi

    resolve_version
    if already_installed; then
        info "already at $VERSION -- nothing to do (--force to reinstall)"
        return 0
    fi
    fetch_and_verify
    extract_tree
    link_tree
    ensure_manager
    link_manager
    check_package_install
    advise_qemu
    advise_python
    maybe_toolchain
    maybe_prune
    check_path

    printf '\n  %s %s installed.\n' "$LABEL" "$VERSION"
    printf '    axl --help                 host commands\n'
    printf '    axl-cc hello.c -o hello.efi\n'
    printf '    sh install.sh --uninstall  remove it again\n'

    # SAY WHICH HALF DID NOT HAPPEN. The banner above is true -- the SDK is
    # installed -- and printing it alone after a failed toolchain step would be
    # the same silent-downgrade defect this whole feature exists to remove,
    # just relocated. aa64 has no host fallback at all (AXL_TOOLCHAIN=axl), so
    # a stale compiler there is a hard failure at the next build, not a
    # degradation.
    if [ "$TOOLCHAIN_FAILED" -ne 0 ]; then
        printf '\n  BUT the %s cross toolchain did NOT update.\n' "$TOOLCHAIN"
        printf '    The SDK above is installed and usable; axl-cc will build\n'
        printf '    with whatever toolchain is already on this machine.\n'
        printf '    Finish it with:  axl toolchain install %s\n' "$TOOLCHAIN"
        # BOTH CALLERS AGAIN: `axl update --no-toolchain` is the answer for the
        # verb, and omitting --toolchain is the answer for a direct run. Naming
        # only the first told a first-time install.sh user to type a flag on a
        # command they had not run.
        printf '    Skip it next time with:  axl update --no-toolchain\n'
        printf '      (or, running install.sh directly, omit --toolchain)\n'
        return 1
    fi
}

main "$@"
