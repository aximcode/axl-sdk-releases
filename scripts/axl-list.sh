#!/bin/bash
# axl-list.sh -- print the build's named lists (scripts/axl-lists.conf).
#
# Usage:
#   scripts/axl-list.sh NAME...      one line per NAME: its words, one space
#                                    apart -- what `make -s print-NAME` printed
#   scripts/axl-list.sh --names      every defined NAME, one per line
#   AXL_LISTS_FILE=<file> ...        read another file (the self-test's
#                                    fixtures); default scripts/axl-lists.conf
#
# Exit 0 with every NAME printed; 2 on a NAME the file does not define, or on
# a line it cannot read. It never prints a partial answer: a list that came
# back short or empty is how a packaging step ships too little, so a reader
# that cannot answer says so and prints nothing on stdout.

set -euo pipefail
# No pathname expansion: the words are split unquoted below, and `*.c` must be
# refused as a word, never expanded against the caller's directory.
set -f

FILE="${AXL_LISTS_FILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/axl-lists.conf}"

die() { echo "axl-list.sh: $*" >&2; exit 2; }

[[ -r "$FILE" ]] || die "cannot read $FILE"
[[ $# -gt 0 ]] || die "usage: axl-list.sh NAME... | --names"

declare -A LIST=()
ORDER=()
logical="" lineno=0 start=0
# _define <line-number> <logical line>
_define() {
    local n="$1" line="$2" name rhs out=() w ref
    if [[ ! "$line" =~ ^([A-Z][A-Z0-9_]*)[[:space:]]*=[[:space:]]*(.*)$ ]]; then
        die "$FILE:$n: not 'NAME = words': $line"
    fi
    name="${BASH_REMATCH[1]}"
    rhs="${BASH_REMATCH[2]}"
    [[ -z "${LIST[$name]+set}" ]] || die "$FILE:$n: $name defined twice"
    # Words only: a make construct this reader does not implement (`:=`,
    # `+=`, functions, `$X`, a trailing `# comment`, a glob) must not be read
    # as literal words -- make would read each differently.
    for w in $rhs; do
        if [[ "$w" =~ ^\$\(([A-Z][A-Z0-9_]*)\)$ ]]; then
            ref="${BASH_REMATCH[1]}"
            [[ -n "${LIST[$ref]+set}" ]] || die "$FILE:$n: $name uses \$($ref) before it is defined"
            # shellcheck disable=SC2206  # a defined list: words, no globs
            out+=(${LIST[$ref]})
        elif [[ "$w" == *[\$\(\)=:\;\'\"\`\\#*?[]* ]]; then
            die "$FILE:$n: $name: '$w' is not a plain word or \$(NAME)"
        else
            out+=("$w")
        fi
    done
    (( ${#out[@]} > 0 )) || die "$FILE:$n: $name is empty"
    LIST[$name]="${out[*]}"
    ORDER+=("$name")
}

while IFS= read -r raw || [[ -n "$raw" ]]; do
    lineno=$((lineno + 1))
    if [[ -z "$logical" ]]; then
        [[ "$raw" =~ ^[[:space:]]*(#|$) ]] && continue
        start=$lineno
    fi
    if [[ "$raw" == *\\ ]]; then
        logical+="${raw%\\} "
        continue
    fi
    logical+="$raw"
    _define "$start" "$logical"
    logical=""
done < "$FILE"
[[ -z "$logical" ]] || die "$FILE:$start: a continued line runs off the end of the file"

if [[ "$1" == --names ]]; then
    printf '%s\n' "${ORDER[@]}"
    exit 0
fi

out=()
for name in "$@"; do
    [[ -n "${LIST[$name]+set}" ]] || die "no list named '$name' in $FILE (it defines: ${ORDER[*]})"
    out+=("${LIST[$name]}")
done
printf '%s\n' "${out[@]}"
