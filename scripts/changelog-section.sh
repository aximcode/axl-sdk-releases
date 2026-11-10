#!/bin/bash
# changelog-section.sh -- print one version's CHANGELOG.md section: the
# release notes.
#
# release.yml publishes exactly this as the GitHub release body, and
# test-release-gate.sh runs it on a fixture to prove what cut-release.sh
# writes into a section (the --tag-untested note) reaches those notes. One
# copy of the extraction, so the test reads what the workflow publishes rather
# than a re-statement of it.
#
# Matches `## <version>` followed by a space or end of line, so 0.12.0 does not
# match 0.12.0-rc1. Versions hold no regex metacharacters beyond `.`, which
# matches itself anyway.
#
# Usage: scripts/changelog-section.sh <version> [changelog]   (default CHANGELOG.md)
# Exit:  0 printed the section; 1 no such section (the headings are listed on
#        stderr); 2 usage or an unreadable file.
set -uo pipefail

V="${1:-}"
F="${2:-CHANGELOG.md}"
[[ -n "$V" ]] || { echo "usage: $0 <version> [changelog]" >&2; exit 2; }
[[ -r "$F" ]] || { echo "ERROR: cannot read $F" >&2; exit 2; }

section=$(awk -v ver="$V" '
  /^## / {
    if (in_section) exit
    if ($0 ~ ("^## " ver "( |$)")) { in_section = 1; next }
  }
  in_section { print }
' "$F")
if [[ -z "$section" ]]; then
    echo "ERROR: no CHANGELOG section found for v${V}" >&2
    echo "Headings present:" >&2
    grep '^## ' "$F" | head -10 >&2
    exit 1
fi
printf '%s\n' "$section"
