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
    BIN_DIR="${BIN_DIR:-${XDG_BIN_HOME:-$HOME/.local/bin}}"
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

# The prefix is self-contained (§12.3), so removal is rm -rf plus the links.
# Links are RESOLVED rather than guessed, so one pointing elsewhere is safe.
do_uninstall() {
    _target=""
    if [ -n "$VERSION" ]; then
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
    [ -n "$VERSION" ] && return 0
    _vurl="${BASE_URL:-$BASE/latest/download}/VERSION"
    VERSION="$(curl -fsSL "$_vurl" 2>/dev/null | tr -d ' \t\r\n')" \
        || die "could not resolve the latest version from $_vurl"
    [ -n "$VERSION" ] || die "the VERSION asset at $_vurl was empty"
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
    curl -fsSL -o "$WORK/SHA256SUMS" "$DL/SHA256SUMS" \
        || die "could not download $DL/SHA256SUMS"

    ASSET=""; _tried=""
    for _c in $(asset_candidates); do
        _tried="$_tried $_c"
        if sums_line "$WORK/SHA256SUMS" "$_c" >/dev/null; then ASSET="$_c"; break; fi
    done
    [ -n "$ASSET" ] || die "release $VERSION publishes none of:$_tried"

    curl -fsSL -o "$WORK/$ASSET" "$DL/$ASSET" || die "could not download $DL/$ASSET"
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

maybe_toolchain() {
    [ -n "$TOOLCHAIN" ] || return 0
    _it="$PREFIX_ROOT/$DIR/bin/axl-install-toolchain"
    [ -x "$_it" ] || die "--toolchain needs the SDK component, not --host-tools"
    info "installing the $TOOLCHAIN cross toolchain (uses sudo for /opt)"
    ensure "$_it" "$TOOLCHAIN"
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
}

main "$@"
