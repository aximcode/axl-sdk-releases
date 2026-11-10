#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0 builds=io-streams guest=standard
# I/O-model redirect separation: stdout -> `>`, stderr -> `2>`, and
# NEITHER stderr NOR diagnostic logs land in a `>`-redirected stdout file.
source "$(dirname "$0")/common-test.sh"
export TEST_SKIP_RATCHET=1
while [[ $# -gt 0 ]]; do case "$1" in --arch) TEST_ARCH="$2"; shift 2;; *) exit 1;; esac; done
test_setup
declare -A _M=([X64]=x64 [AARCH64]=aa64); _a="${_M[$TEST_ARCH]:-x64}"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" io-streams "$(test_build_prefix "$_a")/tools/hexdump.efi" 2>&1 | tail -2
EFI="$(test_build_dir "$_a")/io-streams.efi"
HEXDUMP="$(test_build_dir "$_a")/tools/hexdump.efi"
if [[ ! -f "$EFI" ]]; then echo "WARN: io-streams.efi not built; skipping."; echo "IO redirect test: SKIP"; exit 0; fi
test_add_efi "$EFI"
test_add_efi "$HEXDUMP"
test_use_nsh
test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 60
test_clean_log
# Each section below gets its OWN BEGIN/END marker pair so the sed windows are
# strictly non-overlapping -- no boundary line is scanned by two windows (a
# prior version shared one marker line, e.g. TYPE_ERR, as both a window's END
# and the next window's START; harmless in practice since the grepped markers
# below never collide with the boundary echo text, but non-overlapping windows
# rule that out structurally instead of relying on that non-collision).
out=$(sed -n '/TYPE_OUT_BEGIN/,/TYPE_OUT_END/p' "$TEST_CLEAN_LOG")
err=$(sed -n '/TYPE_ERR_BEGIN/,/TYPE_ERR_END/p' "$TEST_CLEAN_LOG")
out_has_stdout=$(grep -c 'OUT:stdout' <<<"$out" || true)
out_has_stderr=$(grep -c 'ERR:stderr' <<<"$out" || true)
out_has_log=$(grep -c 'LOG:warn'      <<<"$out" || true)
err_has_stderr=$(grep -c 'ERR:stderr' <<<"$err" || true)
err_has_log=$(grep -c 'LOG:warn'      <<<"$err" || true)
# Each hexdump.c row is a fixed-width "OFFSET: <16-byte hex, 40 cols> <ascii>"
# regardless of how many bytes that row actually holds (padding covers
# short/last rows) -- 10 (offset+": ") + 40 (hex) + 1 (separator) = 51
# columns before the ASCII column starts. Concatenating column 52+ across
# every row (in file order) reconstructs the byte stream as text WITHOUT
# the row-boundary breaks a naive per-line grep would be at the mercy of,
# so the raw marker is found intact even if it straddles a 16-byte row.
hex_out=$(sed -n '/HEX_OUT_BEGIN/,/HEX_OUT_END/p' "$TEST_CLEAN_LOG" | grep -E '^[0-9a-f]{8}: ' | cut -c52- | tr -d '\n')
hex_err=$(sed -n '/HEX_ERR_BEGIN/,/HEX_ERR_END/p'  "$TEST_CLEAN_LOG" | grep -E '^[0-9a-f]{8}: ' | cut -c52- | tr -d '\n')
out_has_raw=$(grep -c 'RAW:err' <<<"$hex_out" || true)
err_has_raw=$(grep -c 'RAW:err' <<<"$hex_err" || true)
# `>` alone: the file has stdout only; stderr and the log are on the console.
# `2>` alone: the file has stderr and the log; stdout alone is on the console.
win() { sed -n "/^$1_BEGIN\$/,/^$1_END\$/p" "$TEST_CLEAN_LOG"; }
cnt() { grep -c -- "$1" <<<"$2" || true; }
only_out=$(win ONLY_OUT); out1=$(win TYPE_OUT1); only_err=$(win ONLY_ERR); err2=$(win TYPE_ERR2)
split="out1{stdout=$(cnt OUT:stdout "$out1") stderr=$(cnt ERR:stderr "$out1") log=$(cnt LOG:warn "$out1")}"
split+=" console{stderr=$(cnt ERR:stderr "$only_out") log=$(cnt LOG:warn "$only_out")}"
split+=" err2{stdout=$(cnt OUT:stdout "$err2") stderr=$(cnt ERR:stderr "$err2") log=$(cnt LOG:warn "$err2")}"
split+=" console{stdout=$(cnt OUT:stdout "$only_err") stderr=$(cnt ERR:stderr "$only_err") log=$(cnt LOG:warn "$only_err")}"
want_split="out1{stdout=1 stderr=0 log=0} console{stderr=1 log=1} err2{stdout=0 stderr=1 log=1} console{stdout=1 stderr=0 log=0}"
echo "Split: $split"
if [[ "$split" != "$want_split" ]]; then
  echo "  want: $want_split"
  echo "IO redirect test: FAIL"; exit 1
fi
# One window's counts of the three text markers.
counts() { local w; w=$(win "$1"); echo "$1{stdout=$(cnt OUT:stdout "$w") stderr=$(cnt ERR:stderr "$w") log=$(cnt LOG:warn "$w")}"; }
sinks="$(counts NUL_ERR) $(counts NUL_OUT) $(counts ENV_ERR) $(counts ENV_ERR_VAL) $(counts ENV_OUT) $(counts ENV_OUT_VAL)"
want_sinks="NUL_ERR{stdout=1 stderr=0 log=0} NUL_OUT{stdout=0 stderr=1 log=1}"
want_sinks+=" ENV_ERR{stdout=1 stderr=0 log=0} ENV_ERR_VAL{stdout=0 stderr=1 log=1}"
want_sinks+=" ENV_OUT{stdout=0 stderr=1 log=1} ENV_OUT_VAL{stdout=1 stderr=0 log=0}"
echo "Sinks: $sinks"
# The raw window, whole lines, the leak verdict's clock as T.
rawcon=$(win RAWCON | sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)? \[/T [/')
want_rawcon='RAWCON_BEGIN
RAW:out
RAW:err
T [INFO]  mem: no leaks detected
RAWCON_END'
if [[ "$rawcon" != "$want_rawcon" ]]; then
  echo "Raw to the console: want exactly"; printf '%s\n' "$want_rawcon" | sed 's/^/    | /'
  echo "  got"; printf '%s\n' "$rawcon" | cat -v | sed 's/^/    | /'
  echo "IO redirect test: FAIL"; exit 1
fi
echo "Raw to the console: exact"
rawsplit=$(win RAWSPLIT | sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)? \[/T [/')
want_rawsplit='RAWSPLIT_BEGIN
abcd
T [INFO]  mem: no leaks detected
RAWSPLIT_END'
if [[ "$rawsplit" != "$want_rawsplit" ]]; then
  echo "Raw UTF-16 in two writes: want exactly"; printf '%s\n' "$want_rawsplit" | sed 's/^/    | /'
  echo "  got"; printf '%s\n' "$rawsplit" | cat -v | sed 's/^/    | /'
  echo "IO redirect test: FAIL"; exit 1
fi
echo "Raw UTF-16 in two writes: exact"
if [[ "$sinks" != "$want_sinks" ]]; then
  echo "  want: $want_sinks"
  echo "IO redirect test: FAIL"; exit 1
fi
echo "Results: out{stdout=$out_has_stdout stderr=$out_has_stderr log=$out_has_log raw=$out_has_raw} err{stderr=$err_has_stderr log=$err_has_log raw=$err_has_raw}"
# GREEN: stdout file has ONLY stdout; stderr file has stderr + logs + raw stderr.
if [[ "$out_has_stdout" -ge 1 && "$out_has_stderr" -eq 0 && "$out_has_log" -eq 0 && "$out_has_raw" -eq 0 \
      && "$err_has_stderr" -ge 1 && "$err_has_log" -ge 1 && "$err_has_raw" -ge 1 ]]; then
  echo "IO redirect test: OK"; exit 0
else
  echo "IO redirect test: FAIL"; exit 1
fi
