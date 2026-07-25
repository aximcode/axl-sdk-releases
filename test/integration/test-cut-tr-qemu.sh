#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# test-cut-tr-qemu.sh — cut(1) and tr(1) match GNU behavior for the common
# option set, driven over UEFI-Shell stdin pipes (the real use: `... | cut | tr`).
#
# Every expected value here was captured from the host GNU cut/tr, so the test
# asserts GNU PARITY, not merely "what our tool happens to emit". Each case
# frames its output between unique `MARK-TAG-A` / `-B` sentinels so the exact
# bytes (including an EMPTY result, e.g. cut -s suppressing a line) are
# extracted unambiguously; the trailing shell `\r` is stripped, then the
# result is compared for EXACT equality.
#
# The input is fed as `echo X | tool` — the Shell delivers that pipe as UCS-2,
# so this also guards that cut/tr decode it (a raw read stops at the first NUL
# byte, the bug this test first caught).
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's count).
#
# Usage: ./test/integration/test-cut-tr-qemu.sh [X64|AARCH64|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

# Cases: "TAG<TAB>shell command<TAB>expected exact output".
# Keep TAGs unique and free of regex/sed metacharacters.
read_cases() {
cat <<'CASES'
CUTF24	echo a:b:c:d | cut -d: -f2,4	b:d
CUTFOPEN	echo a:b:c:d | cut -d: -f2-	b:c:d
CUTREORDER	echo a:b:c:d | cut -d: -f3,1	a:c
CUTCHAR	echo hello | cut -c2-4	ell
CUTBYTE	echo hello | cut -b1,3,5	hlo
CUTCOMP	echo a:b:c | cut -d: -f2 --complement	a:c
CUTCHARCOMP	echo hello | cut -c2-4 --complement	ho
CUTOD	echo a:b:c | cut -d: -f1,3 --output-delimiter=-	a-c
CUTNODELIM	echo nodelim | cut -d: -f2	nodelim
CUTSUPPRESS	echo nodelim | cut -d: -f2 -s
TRUP	echo hello | tr a-z A-Z	HELLO
TRDEL	echo hello | tr -d l	heo
TRSQ	echo aabbcc | tr -s a-c	abc
TRSQAB	echo aaabbb | tr -s ab	ab
TRSPACE	echo hello world | tr " " "_"	hello_world
TRCLASS	echo hello | tr [:lower:] [:upper:]	HELLO
TRXLT	echo abcdef | tr abc xyz	xyzdef
TRRANGE	echo abcdef | tr b-d X	aXXXef
TRDS	echo aabbcc | tr -ds a b	bcc
TRCMULTI	echo bcd | tr -c \000-\141 xy	xyy
TROCTAL	echo hi | tr i \055	h-
CASES
}

overall_fail=0

run_one() {
    local arch="$1" native_arch out cutefi trefi log nsh pass fail
    case "$arch" in
        X64)     native_arch="x64";  out="$PROJECT_DIR/out/native-x64" ;;
        AARCH64) native_arch="aa64"; out="$PROJECT_DIR/out/native-aa64" ;;
    esac
    cutefi="$out/tools/cut.efi"
    trefi="$out/tools/tr.efi"

    echo "=== cut/tr ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
        "out/native-$native_arch/tools/cut.efi" \
        "out/native-$native_arch/tools/tr.efi" 2>&1 | tail -1

    # Build the nsh: each case wrapped in unique sentinels.
    nsh="$(mktemp)"
    {
        echo "@echo -off"
        while IFS=$'\t' read -r tag cmd _exp; do
            [[ -z "$tag" ]] && continue
            echo "echo MARK-$tag-A"
            echo "$cmd"
            echo "echo MARK-$tag-B"
        done < <(read_cases)
        echo "reset -s"
    } > "$nsh"

    log="$(mktemp)"
    timeout 150s "$RUN_QEMU" --arch "$arch" --timeout 90 --nsh "$nsh" \
        "$cutefi" --extra "$trefi" > "$log" 2>&1 || true
    rm -f "$nsh"
    # Normalize: drop CR and the async [INFO] mem-leak lines that can interleave.
    tr -d '\r' < "$log" | grep -v "mem: no leaks detected" > "$log.norm"

    pass=0
    fail=0
    while IFS=$'\t' read -r tag cmd exp; do
        [[ -z "$tag" ]] && continue
        # Extract the lines strictly between "<<TAG" and "TAG>>".
        local got
        got="$(sed -n "/^MARK-$tag-A\$/,/^MARK-$tag-B\$/p" "$log.norm" \
                | sed '1d;$d' | tr '\n' '\001')"
        got="${got%$'\001'}"          # drop trailing record sep
        local want="${exp//$'\n'/$'\001'}"
        if [[ "$got" == "$want" ]]; then
            pass=$((pass + 1))
        else
            echo "  FAIL $tag: want [${want//$'\001'/\\n}] got [${got//$'\001'/\\n}]"
            echo "       cmd: $cmd"
            fail=$((fail + 1))
        fi
    done < <(read_cases)

    rm -f "$log" "$log.norm"
    echo "  cut/tr: $pass passed, $fail failed ($arch)"
    if [[ $fail -gt 0 ]]; then
        overall_fail=$((overall_fail + 1))
    fi
    echo
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
done

if (( overall_fail > 0 )); then
    echo "$overall_fail cut/tr arch(es) failed"
    exit 1
fi
echo "All cut/tr checks passed."
exit 0
