#!/bin/bash
# make-source-snapshot.sh — assemble the public source snapshot.
#
# WHY THIS EXISTS. `aximcode/axl-sdk-releases` is public; this repo is not.
# Every release pushes a squashed source snapshot there. The selection rule was
# "everything tracked, minus .github/" -- one exclusion, for a mechanical
# reason, not an editorial one -- so every tracked document was public,
# including internal working notes, and nobody had decided that
# (AXL-Distribution-Design.md §15).
#
# The exclusions and the assembly live HERE rather than inline in release.yml
# so that scripts/check-snapshot-clean.py can be run against a real snapshot on
# a laptop. An exclusion list nothing can exercise until a release is a list
# whose first failure is public and cannot be recalled.
#
# EXCLUDED BY CLASS, not file by file, so a new file joins the right side by
# where it is put:
#
#   .github/              a fine-grained PAT without `workflow` scope cannot
#                         push workflow files; the push would fail outright.
#                         Mechanical, and the original exclusion.
#   session handoffs      working notes: dead ends, arguments settled and then
#                         reversed, consumer specifics.
#   docs/superpowers/     plans and specs -- the same, for the same reasons.
#   HW-Testing-           an infra runbook naming personal machines and
#     Workflow.md         diagramming a reverse-tunnel topology.
#
# Usage:
#   scripts/make-source-snapshot.sh --out DIR [--from HEAD|worktree]
#
#   --from HEAD       what a release would publish (the default; what
#                     release.yml uses).
#   --from worktree   tracked files as they stand right now, so the check can
#                     run BEFORE the leak is committed.
set -euo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

OUT_DIR=""
FROM="HEAD"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)    OUT_DIR="${2:?--out needs a directory}"; shift 2 ;;
        --out=*)  OUT_DIR="${1#--out=}"; shift ;;
        --from)   FROM="${2:?--from needs HEAD or worktree}"; shift 2 ;;
        --from=*) FROM="${1#--from=}"; shift ;;
        -h|--help) sed -n '2,38p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "ERROR: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
[[ -n "$OUT_DIR" ]] || { echo "ERROR: --out is required" >&2; exit 2; }

mkdir -p "$OUT_DIR"
cd "$SDK_DIR"

case "$FROM" in
    HEAD)
        git archive --format=tar HEAD | tar -xC "$OUT_DIR"
        ;;
    worktree)
        # Tracked paths, with their CURRENT contents. -z/--null throughout, so
        # a path with a space or a newline cannot split into two names.
        git ls-files -z | tar --null -T - -cf - | tar -xC "$OUT_DIR"
        ;;
    *)
        echo "ERROR: --from wants HEAD or worktree, got '$FROM'" >&2; exit 2 ;;
esac

# --- the exclusions (see the header) ---------------------------------------
rm -rf "$OUT_DIR/.github"
rm -rf "$OUT_DIR/docs/superpowers"
rm -f  "$OUT_DIR"/docs/AXL-Session-Handoff-*.md
rm -f  "$OUT_DIR/docs/HW-Testing-Workflow.md"

# A snapshot that came out empty is a broken assembly, not a small release.
# Distinguishing that from "it worked" is the whole reason for a count here:
# every check downstream would otherwise report clean over nothing.
_n=$(find "$OUT_DIR" -type f | wc -l)
if [[ "$_n" -lt 100 ]]; then
    echo "ERROR: snapshot holds only $_n files -- assembly failed" >&2
    exit 1
fi
echo "[make-source-snapshot] $OUT_DIR ($_n files, from $FROM)"
