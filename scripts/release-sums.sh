#!/bin/bash
# release-sums.sh DIR -- write DIR/SHA256SUMS over every other file in DIR.
#
# The ONE writer of the checksum file a release publishes: `sha256sum`
# format ("<hex>  <name>"), sorted by name, every regular file in DIR except
# SHA256SUMS itself. packaging/install.sh verifies each download against it
# and consumers pin from it, so release.yml and every test that stages a
# release directory (test-consumer-install.sh) run this rather than restating
# the line.
#
# Exit 0 on success, 1 when DIR holds no file to sum, 2 on a usage error.
set -euo pipefail

[[ $# -eq 1 && -d "$1" ]] || { echo "usage: release-sums.sh DIR" >&2; exit 2; }
cd "$1"
# One order on every builder: the glob's, in the C locale.
export LC_ALL=C
shopt -s nullglob
files=()
for f in *; do
    [[ -f "$f" && "$f" != SHA256SUMS ]] && files+=("$f")
done
(( ${#files[@]} > 0 )) || { echo "release-sums: $1 holds no files" >&2; exit 1; }
# Into a dotfile first, so the glob above can never pick up a half-written
# SHA256SUMS on a re-run, then renamed.
sha256sum -- "${files[@]}" > .SHA256SUMS.tmp
mv -f .SHA256SUMS.tmp SHA256SUMS
echo "release-sums: wrote $1/SHA256SUMS (${#files[@]} files)"
