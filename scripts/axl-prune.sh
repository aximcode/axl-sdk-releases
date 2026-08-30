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
PREFIX="$(dirname "$(dirname "$SCRIPT_DIR")")"
ROOT="$(dirname "$PREFIX")"
CONF="$PREFIX/share/axl/axl-toolchains.conf"

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
            echo "Removes superseded SDK roots beside $PREFIX and superseded"
            echo "toolchain roots beside the ones the manifest names."
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

# prune_family <dir> <regex anchored at the family stem> <keep-in-addition...>
#
# Candidates are sorted by version (sort -V) and the newest $KEEP are kept.
# Everything protected is passed in explicitly rather than filtered afterwards,
# so a protected path can never be printed as "would remove".
prune_family() {
    local base="$1" re="$2"; shift 2
    local protected=("$@")
    local -a cands=()
    local d p skip
    for d in "$base"/*; do
        [[ -d "$d" ]] || continue
        [[ "$(basename "$d")" =~ $re ]] || continue
        skip=0
        for p in "${protected[@]}"; do
            [[ -n "$p" && "$d" == "$p" ]] && { skip=1; break; }
        done
        [[ $skip -eq 1 ]] && continue
        cands+=("$d")
    done
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
# Anchored, and a DIGIT must follow the dash: `axl-sdk-4.3.2` matches while a
# consumer's `axl-sdk-workspace` does not. That one character is the difference
# between pruning our own versions and deleting somebody's project.
prune_family "$ROOT" '^axl-sdk-[0-9]' "$PREFIX" "$CURRENT"

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
        prune_family "$tc_base" "^$(printf '%s' "$stem" | sed 's/[.[\*^$]/\\&/g')[0-9]" "$tc"
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
