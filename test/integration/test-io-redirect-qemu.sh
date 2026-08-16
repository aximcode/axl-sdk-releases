#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# I/O-model redirect separation: stdout -> `>`, stderr -> `2>`, and
# NEITHER stderr NOR diagnostic logs land in a `>`-redirected stdout file.
source "$(dirname "$0")/common-test.sh"
export TEST_SKIP_RATCHET=1
while [[ $# -gt 0 ]]; do case "$1" in --arch) TEST_ARCH="$2"; shift 2;; *) exit 1;; esac; done
test_setup
declare -A _M=([X64]=x64 [AARCH64]=aa64); _a="${_M[$TEST_ARCH]:-x64}"
make -C "$PROJECT_DIR" ARCH="$_a" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} io-streams "$(test_build_prefix "$_a")/tools/hexdump.efi" 2>&1 | tail -2
EFI="$(test_build_dir "$_a")/io-streams.efi"
HEXDUMP="$(test_build_dir "$_a")/tools/hexdump.efi"
if [[ ! -f "$EFI" ]]; then echo "WARN: io-streams.efi not built; skipping."; echo "IO redirect test: SKIP"; exit 0; fi
test_add_efi "$EFI"
test_add_efi "$HEXDUMP"
{
  echo "@echo -off"; echo "fs0:"; echo "cd \\"
  echo "echo R_BEGIN"
  echo "io-streams.efi > out.txt 2> err.txt"
  echo "echo TYPE_OUT_BEGIN"; echo "type out.txt"; echo "echo TYPE_OUT_END"
  echo "echo TYPE_ERR_BEGIN"; echo "type err.txt"; echo "echo TYPE_ERR_END"
  # The err.txt (and out.txt) files are a mix of UCS-2 console text
  # (from ConOut/ConErr's CHAR16 WriteFile) and raw single bytes (from
  # axl_stdout_raw/axl_stderr_raw's direct WriteFile) -- 'type' decodes
  # the whole file as UCS-2, so raw ASCII bytes come out as garbled
  # multi-byte glyphs (each 2 raw bytes fused into one CJK-ish code
  # point). hexdump shows wire truth instead: dump both files and
  # look at the ASCII column for the raw marker (see PROBE 1 in
  # test-shell-pipe.sh for the same "hexdump shows wire truth"
  # precedent with axl_stdin).
  echo "echo HEX_OUT_BEGIN"; echo "hexdump.efi out.txt"; echo "echo HEX_OUT_END"
  echo "echo HEX_ERR_BEGIN"; echo "hexdump.efi err.txt"; echo "echo HEX_ERR_END"
  echo "echo R_DONE"
  echo "reset -s"
} | test_set_startup
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
echo "Results: out{stdout=$out_has_stdout stderr=$out_has_stderr log=$out_has_log raw=$out_has_raw} err{stderr=$err_has_stderr log=$err_has_log raw=$err_has_raw}"
# GREEN: stdout file has ONLY stdout; stderr file has stderr + logs + raw stderr.
if [[ "$out_has_stdout" -ge 1 && "$out_has_stderr" -eq 0 && "$out_has_log" -eq 0 && "$out_has_raw" -eq 0 \
      && "$err_has_stderr" -ge 1 && "$err_has_log" -ge 1 && "$err_has_raw" -ge 1 ]]; then
  echo "IO redirect test: OK"; exit 0
else
  echo "IO redirect test: FAIL"; exit 1
fi
