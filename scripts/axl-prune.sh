#!/bin/bash
# axl-desc: remove superseded SDK and toolchain roots (keeps current + N)
#
# axl-prune.sh — bound what accumulates, on both axes.
#
# WHY. Measured 2026-08-29: 7.1 GB across eight pinned SDK versions in one
# $HOME, every one a source checkout, because with no tarball a version could
# only be pinned by keeping the whole tree. The tarball made that 42 MB
# extracted / 7.2 MB downloaded, so SDK roots are now cheap -- but the
# TOOLCHAINS are not: 500 MB for ARM's and 239 MB for ours, 739 MB per
# generation, and `install-toolchain.sh --prefix` just made them installable in
# more places. With 12 releases in 17 days, "it will not add up" is not an
# argument that survives contact.
#
# POLICY: current + one previous (AXL-Distribution-Design.md §12.4). The value
# of an old root is a FAST ROLLBACK when a version bump breaks a build; beyond
# one, a pinned checksum plus a 7.2 MB download is cheaper than storage.
#
# WHY IT IS SAFE TO DELETE AT ALL: §12.3's contract -- a prefix writes nothing
# outside itself, so removing the directory is a complete uninstall. That is
# asserted by test-sdk-selfcontained.sh, not assumed here.
#
# THREE THINGS ARE NEVER REMOVED, and they are the whole design:
#
#   1. The running prefix. Pruning what you are executing from is the one
#      removal that cannot be undone by re-downloading, because it takes the
#      tool that would do the re-download with it.
#   2. Whatever `current` resolves to. Something else on the machine is
#      pointing at it.
#   3. Anything not identifiably ours. The install root is shared -- /opt has
#      other people's software in it -- so candidates must match a name we
#      generate, anchored, with a version after it. `axl-sdk-*` alone would
#      match a consumer's `axl-sdk-workspace`; the toolchain families come
#      from the staged manifest rather than a hardcoded /opt, for the same
#      reason the self-containment test reads them from there.
#
# Usage: axl prune [--keep N] [--dry-run]
set -uo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "$0")" && pwd)"      # <prefix>/libexec/axl

# GUARDED: test-axl-prune.sh stages this script on its own, with no
# axl-common.sh beside it. An unguarded source printed two errors per
# invocation there and was survived only by the absence of `set -e`.
[[ -r "$SCRIPT_DIR/axl-common.sh" ]] && source "$SCRIPT_DIR/axl-common.sh"

# Answered before anything else parses argv, so `--version` never has to
# survive a positional-hungry option loop. See axl-common.sh.
# Guarded for the same reason the source above is: this function comes from
# axl-common.sh, and a tree without it should not answer --version with a
# "command not found" on every invocation.
if command -v axl_handle_version >/dev/null 2>&1; then
    axl_handle_version "axl-prune" "$@" && exit 0
fi
PREFIX="$(dirname "$(dirname "$SCRIPT_DIR")")"
ROOT="$(dirname "$PREFIX")"
CONF="$PREFIX/share/axl/axl-toolchains.conf"
# THE MANAGER SHIPS NO MANIFEST. make-host-tools-tarball.sh writes
# share/axl/version and nothing else, and since §20 M2 `axl` is linked out of
# the manager -- so a stock `axl prune` looked for the manifest in a prefix
# that never has one and silently skipped every toolchain root. The families
# are a property of the SDK IN USE anyway, not of the manager, so read the
# current SDK's copy when this prefix has none.
if [[ ! -r "$CONF" && -r "$ROOT/axl-sdk/share/axl/axl-toolchains.conf" ]]; then
    CONF="$ROOT/axl-sdk/share/axl/axl-toolchains.conf"
fi

# The file install-toolchain.sh leaves in every root it installs (§21).
RECEIPT=".axl-receipt"

KEEP=1
DRY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --keep)     KEEP="${2:?--keep needs a count}"; shift 2 ;;
        --keep=*)   KEEP="${1#--keep=}"; shift ;;
        --dry-run|-n) DRY=1; shift ;;
        -h|--help)
            echo "Usage: axl prune [--keep N] [--dry-run]"
            echo ""
            echo "Removes superseded SDK roots beside $PREFIX, superseded"
            echo "manager (host-tools) roots, and superseded toolchain roots"
            echo "beside the ones the manifest names."
            echo "Keeps the newest N besides the running prefix (default 1),"
            echo "never the running prefix, never what 'current' points at,"
            echo "and never a directory that is not identifiably ours."
            exit 0 ;;
        *) echo "axl prune: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
if ! [[ "$KEEP" =~ ^[0-9]+$ ]]; then
    echo "axl prune: --keep needs a non-negative integer, got '$KEEP'" >&2
    exit 2
fi

# Resolve `current` if there is one, so its target is protected. -e rather than
# -L: a dangling symlink protects nothing and should not stop the run.
CURRENT=""
if [[ -e "$ROOT/axl-sdk" ]]; then
    CURRENT="$(cd -P "$ROOT/axl-sdk" 2>/dev/null && pwd)"
fi

REMOVED=0

# prune_family <dir> <need-receipt 0|1> <regex at the family stem> <protected...>
#
# NEED-RECEIPT is §21, and the asymmetry is deliberate. `axl-sdk-<semver>` and
# `axl-sdk-host-tools-<ver>` are OUR namespace by construction -- nothing else
# creates those names -- so a pattern is a safe licence to delete. A toolchain
# root is not: ARM publishes its toolchains as `arm-gnu-toolchain-<ver>-...`,
# so the family stem derived from the manifest also matches a developer's own
# unpacked copy of a DIFFERENT target triple. §12.5 required "only roots
# recorded in a manifest we wrote" from the start and the code never did it;
# measured, two foreign arm-none-eabi trees made the older one a candidate,
# and they also crowded out the generation of OURS the keep-one-previous rule
# is supposed to retain.
#
# Fails safe: no receipt means not ours, so it is left alone.
#
# Candidates are sorted by version (sort -V) and the newest $KEEP are kept.
# Everything protected is passed in explicitly rather than filtered afterwards,
# so a protected path can never be printed as "would remove".
prune_family() {
    local base="${1%/}" need_receipt="$2" re="$3"; shift 3
    local protected=("$@")
    # STRIP THE TRAILING SLASH, or every protection silently stops matching.
    # With base=/ the glob below yields `//tmp` while $PREFIX, the `current`
    # target and the toolchain path are all single-slash, so `[[ "$d" == "$p" ]]`
    # is false for paths that ARE the same directory -- and the running prefix,
    # the current marker and the toolchain IN USE all become candidates for
    # `rm -rf`. The SDK and manager halves are additionally guarded by the
    # versioned-name check, but the toolchain half reaches here with whatever
    # `dirname` of the manifest's path gives, which can be `/`.
    [[ -n "$base" ]] || return 0
    local -a cands=() unowned=()
    local d p skip
    for d in "$base"/*; do
        [[ -d "$d" ]] || continue
        [[ "$(basename "$d")" =~ $re ]] || continue
        skip=0
        for p in "${protected[@]}"; do
            [[ -n "$p" && "${d%/}" == "${p%/}" ]] && { skip=1; break; }
        done
        [[ $skip -eq 1 ]] && continue
        # PROTECTED FIRST, then ownership. The other order announced the
        # manifest's OWN toolchain -- the compiler in use, named in a file we
        # wrote -- as "not ours", which reads as the SDK failing to recognise
        # its own install.
        #
        # `!= "0"` and not `-eq 1`: `-eq` evaluates its operand as ARITHMETIC,
        # so an empty or non-numeric parameter is false and the gate silently
        # turns OFF -- falling toward `rm -rf`. The comment above promises the
        # opposite, so garbage has to mean "require a receipt".
        if [[ "$need_receipt" != "0" && ! -r "$d/$RECEIPT" ]]; then
            unowned+=("$d")
            continue
        fi
        cands+=("$d")
    done
    if [[ ${#unowned[@]} -gt 0 ]]; then
        # SAY WHAT WAS DONE, NOT WHY IT WAS DONE. This header used to read
        # "not installed by us, so not ours to remove" and the note under it
        # then asked "(ours, but installed before receipts existed?)" -- one
        # message stating a fact and retracting it three lines later. On a
        # long-lived box the retraction is the true half: the roots skipped
        # here were /opt/x86_64-elf-gcc-14.3.0-axl and -axl2, our own naming
        # convention, installed before receipts existed.
        #
        # The old note's remedy could not work either. It said to re-run the
        # installer to re-mark the root, and the installer does re-mark -- but
        # only the root the MANIFEST names, which is the CURRENT version. The
        # roots this message is printed about are the SUPERSEDED ones, and no
        # invocation of it will ever claim them.
        echo "  skipped ${#unowned[@]} root(s) under $base with no $RECEIPT --" \
             "not removing them"
        if [[ $DRY -eq 1 ]]; then
            for d in "${unowned[@]}"; do echo "      $d"; done
            echo "      A root carries no receipt if we did not install it, or if it"
            echo "      predates receipts (v4.6.0 and earlier). Prune removes only what"
            echo "      it can prove it installed, so these stay until you remove them."
        fi
    fi
    [[ ${#cands[@]} -gt 0 ]] || return 0

    # Newest last, so the tail is what we keep.
    local -a sorted=()
    while IFS= read -r d; do sorted+=("$d"); done \
        < <(printf '%s\n' "${cands[@]}" | sort -V)

    local n=${#sorted[@]}
    local drop=$(( n - KEEP ))
    [[ $drop -gt 0 ]] || return 0

    local i
    for (( i = 0; i < drop; i++ )); do
        if [[ $DRY -eq 1 ]]; then
            echo "  would remove  ${sorted[$i]}"
        else
            echo "  removing      ${sorted[$i]}"
            rm -rf -- "${sorted[$i]}"
        fi
        REMOVED=$(( REMOVED + 1 ))
    done
}

echo "[axl prune] root: $ROOT   keep: $KEEP$( [[ $DRY -eq 1 ]] && echo '   (dry run)')"

# --- SDK roots -------------------------------------------------------------
# ONLY when the running prefix is itself a versioned root. A package install
# puts us under /usr, so ROOT resolves to `/` -- the anchored pattern below
# finds nothing there and the run is harmless, but the banner above has just
# announced "root: /" and a user should not have to reason about an anchor to
# know that is safe. Say which half ran instead of leaving it to inference.
#
# The pattern is anchored, and a DIGIT must follow the dash: `axl-sdk-4.3.2`
# matches while a consumer's `axl-sdk-workspace` does not. That one character
# is the difference between pruning our own versions and deleting somebody's
# project.
# TWO SHAPES QUALIFY, and the second arrived with §20 M2. `axl` is linked out
# of the MANAGER root, so `axl-sdk-host-tools-<ver>` is the prefix a stock
# `axl prune` runs from -- and it does not match `^axl-sdk-[0-9]`, because the
# character after the dash is `h`. The guard therefore rejected the manager as
# "not a versioned root" and pruned nothing at all, while still exiting 0.
_pfx_name="$(basename "$PREFIX")"
if [[ "$_pfx_name" =~ ^axl-sdk-[0-9] ]] \
   || [[ "$_pfx_name" =~ ^axl-sdk-host-tools-[0-9] ]]; then
    prune_family "$ROOT" 0 '^axl-sdk-[0-9]' "$PREFIX" "$CURRENT"

    # --- manager roots ------------------------------------------------------
    # §20 keeps the manager OUTSIDE `^axl-sdk-[0-9]` so `axl use` and `axl
    # prune` can never move or remove the thing that manages versions. That
    # invariant is about the CURRENT manager, not about every generation of
    # one, and with nothing walking the family they accumulated without bound.
    #
    # Its own `current` marker, per component -- `$ROOT/axl-sdk` is the SDK's
    # and resolves to something in the other family entirely. $PREFIX is passed
    # separately because prune EXECUTES from this tree's libexec: pruning the
    # root you are running out of would delete the script mid-run and take the
    # `axl` on PATH with it, and that root is not always the current one.
    _ht_current=""
    if [[ -e "$ROOT/axl-sdk-host-tools" ]]; then
        _ht_current="$(cd -P "$ROOT/axl-sdk-host-tools" 2>/dev/null && pwd)"
    fi
    prune_family "$ROOT" 0 '^axl-sdk-host-tools-[0-9]' "$PREFIX" "$_ht_current"
else
    echo "  SDK roots:    not applicable -- $PREFIX is not a versioned root"
    echo "                Something else placed this tree, so pruning beside it"
    echo "                could delete files this script does not own. Whatever"
    echo "                installed it manages its versions."
fi

# --- toolchain roots -------------------------------------------------------
# The families come from the staged manifest: the directory that is CURRENT is
# whatever it names, and its siblings sharing the family stem are the
# superseded generations. Hardcoding /opt here would miss a relocated toolchain
# entirely -- and silently, which is the worse half.
if [[ -r "$CONF" ]]; then
    # shellcheck source=/dev/null
    . "$CONF"
    for tc in "${AXL_AA64_TOOLCHAIN_DIR:-}" "${AXL_X64_TOOLCHAIN_DIR:-}"; do
        [[ -n "$tc" ]] || continue
        tc_base="$(dirname "$tc")"
        tc_name="$(basename "$tc")"
        # Family stem: the name up to the first version-looking component, so
        # arm-gnu-toolchain-14.3.rel1-... yields `arm-gnu-toolchain-` and
        # x86_64-elf-gcc-14.3.0-axl3 yields `x86_64-elf-gcc-`.
        stem="$(printf '%s' "$tc_name" | sed -E 's/-[0-9].*$//')-"
        prune_family "$tc_base" 1 "^$(printf '%s' "$stem" | sed 's/[.[\*^$]/\\&/g')[0-9]" "$tc"
    done
else
    echo "  (no manifest at $CONF -- skipping toolchain roots)"
fi

if [[ $REMOVED -eq 0 ]]; then
    echo "[axl prune] nothing to remove"
elif [[ $DRY -eq 1 ]]; then
    echo "[axl prune] $REMOVED would be removed; re-run without --dry-run"
else
    echo "[axl prune] removed $REMOVED"
fi
