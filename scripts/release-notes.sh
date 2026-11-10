#!/bin/bash
# release-notes.sh -- the GitHub release body for one version: its CHANGELOG
# section, a rule, and the links footer. release.yml publishes exactly this
# file (body_path:), and test-release-gate.sh pins it.
#
# WHY A FILE, AND WHY A CAP. v5.0.0's first publish failed. Its section was
# 228 KB and went to the publish action INLINE (`body:`), and an inline input
# reaches the action as one environment string -- over Linux's 128 KB limit
# on one (MAX_ARG_STRLEN), so the runner could not even start node ("Argument
# list too long"). A file removes that limit, but GitHub's own cap on a
# release body is 125,000 characters, and that one is not optional.
#
# So a section that does not fit is CUT at an entry boundary (a line starting
# `- ` or `#`), never inside an entry, keeping the first entries -- Breaking
# comes first; never inside a code fence -- and says so in a note linking the
# full CHANGELOG.md, which
# the releases repo carries at every tag. The cap is counted in BYTES (LC_ALL=C):
# a byte count can only over-count GitHub's characters, so the body always fits.
#
# Usage: scripts/release-notes.sh <version> [changelog]   (default CHANGELOG.md)
# Env:   RELEASES_REPO (default aximcode/axl-sdk-releases)
# Exit:  as scripts/changelog-section.sh: 0 printed; 1 no such section;
#        2 usage or an unreadable file.
set -uo pipefail
export LC_ALL=C

# GitHub's cap is 125,000 characters; 5,000 below it is headroom against any
# normalisation on its side (CRLF line endings would add one per line, ~1,900
# on 5.0.0's notes). A cut costs a few entries; a failed publish costs a re-cut.
MAX_BYTES=120000
V="${1:-}"
F="${2:-CHANGELOG.md}"
REPO="${RELEASES_REPO:-aximcode/axl-sdk-releases}"
[[ -n "$V" ]] || { echo "usage: $0 <version> [changelog]" >&2; exit 2; }

section=$("$(dirname "$0")/changelog-section.sh" "$V" "$F") || exit $?
# Drop the blank lines that open the section (under its heading) and close it,
# so the body starts with content and the rule follows exactly one blank line.
section=$(printf '%s\n' "$section" | sed -e '/[^[:space:]]/,$!d' -e :a -e '/^[[:space:]]*$/{$d;N;ba' -e '}')

DL="https://github.com/$REPO/releases/download/v$V"
tail_text=$'\n\n---\n\n'"📦 **Install / quick start:** see the [README on the releases repo](https://github.com/$REPO#install-the-sdk).
📚 **API reference:** [axl.aximcode.com](https://axl.aximcode.com/).
🔒 **Verify downloads:** [\`SHA256SUMS\`]($DL/SHA256SUMS).
📂 **Source archive:** [\`v$V.tar.gz\`](https://github.com/$REPO/archive/refs/tags/v$V.tar.gz)."

# ${#x} counts bytes under LC_ALL=C. +1 for the newline printf adds at the end.
if (( ${#section} + ${#tail_text} + 1 <= MAX_BYTES )); then
    printf '%s%s\n' "$section" "$tail_text"
    exit 0
fi

# An entry is a column-0 `- ` line outside a code fence -- the same rule the
# cut uses, so the note's counts describe what was actually cut.
count_entries() {
    awk '/^[[:space:]]*(```|~~~)/ { fence = !fence; next }
         !fence && /^- / { n++ }
         END { print n + 0 }'
}
total=$(count_entries <<< "$section")
# The note's length depends on the kept count; budget for the longest it can
# be (kept = total), so the cut never has to be redone.
note_for() {
    printf '*These notes stop after %s of %s entries: a GitHub release holds at most 125,000 characters. Every entry is in [CHANGELOG.md](https://github.com/%s/blob/v%s/CHANGELOG.md).*' \
        "$1" "$total" "$REPO" "$V"
}
widest=$(note_for "$total")
budget=$(( MAX_BYTES - ${#tail_text} - ${#widest} - 3 ))   # "\n\n" before the note, "\n" at the end

# Keep every line before the LAST boundary line that starts within budget:
# what precedes a boundary is whole entries. Then drop trailing blank lines
# and headings left with no entry under them.
kept=$(awk -v budget="$budget" '
    { line[NR] = $0; start[NR] = used; used += length($0) + 1 }
    # Inside a code fence nothing is a boundary: a cut there leaves the fence
    # open and the note and footer render as code.
    /^[[:space:]]*(```|~~~)/ { fence = !fence; next }
    !fence && /^(- |#)/ && start[NR] <= budget { cut = NR }
    END {
        # A section that fits entirely but for the tail lands here too: keep
        # it all if it does.
        if (used <= budget) cut = NR + 1
        last = cut - 1
        while (last > 0 && line[last] ~ /^([[:space:]]*|#.*)$/) last--
        for (i = 1; i <= last; i++) print line[i]
    }' <<< "$section")
nkept=$(count_entries <<< "$kept")
if [[ -n "$kept" ]]; then
    printf '%s\n\n%s%s\n' "$kept" "$(note_for "$nkept")" "$tail_text"
else
    printf '%s%s\n' "$(note_for 0)" "$tail_text"
fi
