#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# Shared-driver stdio bridge — acceptance test.
#
# A resident shared driver's axl_stdin/axl_stdout must reflect the
# LAUNCHING app's shell stdio, not the driver image's (which has none
# of its own — a buffer-loaded DXE driver inherits no shell params).
# axl_shared_driver_locate now installs a backend stdio-bridge protocol
# on every successful locate, carrying the launcher's StdIn/StdOut/StdErr
# handles across the image boundary; the driver's axl_backend_shell_stdin
# consults that bridge when it has no shell params of its own.
#
# stdio-bridge-fix.efi is a thin launcher with stdio-bridge-driver.efi
# embedded. It locates the driver and dispatches a verb:
#   echo     -> driver reads one line from axl_stdin (raw), prints GOT:<line>
#   echotext -> driver reads one line via axl_stdin_text() (UCS-2->UTF-8)
#   emit     -> driver prints DRIVEROUT (the > redirect probe)
#
# startup.nsh drives five cases through the UEFI shell:
#   echo hello |a stdio-bridge-fix.efi echo  -> GOT:hello       (PIPE, raw/|a)
#   echo textpipeinput | stdio-bridge-fix.efi echotext -> GOT:textpipeinput (TEXTPIPE, default |)
#   stdio-bridge-fix.efi echo < in.txt       -> GOT:redirhello  (< REDIRECT)
#   stdio-bridge-fix.efi emit > out.txt; type out.txt -> DRIVEROUT (> PROBE)
#   stdio-bridge-fix.efi echo (no input)     -> GOT:<EOF>       (NO-REGRESSION)
#
# A second launcher, stdio-bridge-self.efi, mirrors a consumer that
# resolves the resident driver ITSELF (warm fast-path: guid + find_guid,
# NOT axl_shared_driver_locate) and calls the public escape hatch
# axl_shared_driver_install_stdio_bridge() before dispatch. Its cases use
# distinct input values (selfhello / selftext / selfredir), scoped to the
# SELF_BEGIN..SELF_DONE window, so they can't be satisfied by the locate
# launcher's tokens.
#
# CORE PASS bar: PIPE (GOT:hello), TEXTPIPE (GOT:textpipeinput via default |),
# < REDIRECT (GOT:redirhello), the no-regression clean EOF (GOT:<EOF>),
# AND the three self-locate cases (GOT:selfhello / selftext / selfredir).
# The > case is an INFORMATIONAL probe — its result decides whether StdOut
# bridging (a later task) is needed; it does NOT gate this test.
#
# Usage: ./test/integration/test-driver-stdio-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

# Opt out of the ratchet — this is an end-to-end scenario, not a unit
# binary whose assertion count feeds the ratchet baseline.
export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    stdio-bridge-fix stdio-bridge-self stdio-bridge-leak 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
LAUNCHER="$NATIVE_DIR/stdio-bridge-fix.efi"
SELF_LAUNCHER="$NATIVE_DIR/stdio-bridge-self.efi"
LEAK_LAUNCHER="$NATIVE_DIR/stdio-bridge-leak.efi"

# Skip-and-warn if the fixture could not be built/staged on this box
# (matches the skip convention of the sibling driver tests).
if [[ ! -f "$LAUNCHER" || ! -f "$SELF_LAUNCHER" || ! -f "$LEAK_LAUNCHER" ]]; then
    echo "WARN: stdio-bridge fixtures not built on this box; skipping."
    echo "Driver stdio-bridge test: SKIP"
    exit 0
fi

test_add_efi "$LAUNCHER"
test_add_efi "$SELF_LAUNCHER"
test_add_efi "$LEAK_LAUNCHER"

# Stage the < redirect input files. The first line is the value the
# driver should echo back through the bridge. in2.txt feeds the
# self-locating launcher's redirect case.
printf 'redirhello\nsecond line ignored\n' > "$TEST_STAGING/in.txt"
printf 'selfredir\nsecond line ignored\n' > "$TEST_STAGING/in2.txt"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    # STALE-BRIDGE / WARM-PATH USE-AFTER-FREE REGRESSION (consumer do.efi):
    #   1. load the driver resident via a NON-stdin verb (clean launcher),
    #   2. a leaker launcher installs the bridge with a PIPE StdIn then exits
    #      WITHOUT uninstalling -> a STALE bridge whose stdin_h is a freed
    #      pipe handle,
    #   3. a warm self-locating launcher reads a pipe. The driver must SKIP
    #      the stale (dead-launcher) bridge and read THIS launcher's input
    #      ("warmpipe") instead of dereferencing the freed handle (#GP/#PF
    #      in Shell.dll — the exact do.efi symptom).
    # Runs FIRST so the stale instance is the OLDEST (returned first by a
    # naive LocateProtocol) — the precise ordering that crashed do.efi.
    echo "echo STALE_BEGIN"
    echo "stdio-bridge-fix.efi emit"
    echo "echo poison |a stdio-bridge-leak.efi"
    echo "echo warmpipe |a stdio-bridge-self.efi echo"
    echo "echo STALE_DONE"
    echo "echo PIPE_BEGIN"
    # `|a` is the UEFI shell's ASCII pipe: it feeds the LHS output to the
    # RHS StdIn as raw 8-bit bytes. The default `|` pipes UCS-2 (UTF-16LE),
    # which axl_stdin (raw bytes) would read as 'h\0e\0...' -> GOT:h.
    echo "echo hello |a stdio-bridge-fix.efi echo"
    echo "echo TEXTPIPE_BEGIN"
    # Default `|` pipes UCS-2; the echotext verb reads via axl_stdin_text(),
    # which decodes it to UTF-8 — so this works WITHOUT the `|a` operator.
    echo "echo textpipeinput | stdio-bridge-fix.efi echotext"
    echo "echo REDIR_BEGIN"
    echo "stdio-bridge-fix.efi echo < in.txt"
    echo "echo EMIT_BEGIN"
    echo "stdio-bridge-fix.efi emit > out.txt"
    echo "echo TYPE_BEGIN"
    echo "type out.txt"
    echo "echo NOINPUT_BEGIN"
    echo "stdio-bridge-fix.efi echo"
    # Self-locating launcher: resolves the resident driver itself (warm
    # fast-path) and installs the bridge via the public escape hatch
    # axl_shared_driver_install_stdio_bridge(). The driver is resident
    # because the locate launcher above already loaded it. Distinct input
    # values keep these GOT: tokens unambiguous from the locate cases.
    echo "echo SELF_BEGIN"
    echo "echo selfhello |a stdio-bridge-self.efi echo"
    echo "echo selftext | stdio-bridge-self.efi echotext"
    echo "stdio-bridge-self.efi echo < in2.txt"
    echo "echo SELF_DONE"
    echo "echo STDIO_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Driver Stdio-Bridge Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log

echo "--- serial log (PIPE_BEGIN .. STDIO_DONE) ---"
sed -n '/PIPE_BEGIN/,/STDIO_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

# Anchored exact-string assertions on the captured serial log.
pipe=$(grep -c '^GOT:hello$' "$TEST_CLEAN_LOG" || true)
textpipe=$(grep -c '^GOT:textpipeinput$' "$TEST_CLEAN_LOG" || true)
redir=$(grep -c '^GOT:redirhello$' "$TEST_CLEAN_LOG" || true)
noinput=$(grep -c '^GOT:<EOF>$' "$TEST_CLEAN_LOG" || true)
done_marker=$(grep -c '^STDIO_DONE' "$TEST_CLEAN_LOG" || true)

# Self-locating launcher (public axl_shared_driver_install_stdio_bridge).
# Scope to the SELF_BEGIN..SELF_DONE window so these can't be satisfied by
# the locate-launcher cases above.
self_section() { sed -n '/SELF_BEGIN/,/SELF_DONE/p' "$TEST_CLEAN_LOG"; }
self_pipe=$(self_section | grep -c '^GOT:selfhello$' || true)
self_textpipe=$(self_section | grep -c '^GOT:selftext$' || true)
self_redir=$(self_section | grep -c '^GOT:selfredir$' || true)

# Stale-bridge regression: after a leaked (dead-launcher) bridge, the warm
# read must produce THIS launcher's input — not crash on the freed handle.
stale_warm=$(sed -n '/STALE_BEGIN/,/STALE_DONE/p' "$TEST_CLEAN_LOG" \
    | grep -c '^GOT:warmpipe$' || true)

# > probe — informational only. Disambiguate the source of DRIVEROUT:
#   - between EMIT_BEGIN and TYPE_BEGIN  => printed to CONSOLE at the emit
#     run; the `>` redirect did NOT capture the driver's StdOut.
#   - between TYPE_BEGIN and NOINPUT_BEGIN => `type out.txt` read it back;
#     the `>` redirect DID capture the driver's StdOut to the file.
emit_console=$(sed -n '/EMIT_BEGIN/,/TYPE_BEGIN/p' "$TEST_CLEAN_LOG" \
    | grep -c '^DRIVEROUT$' || true)
type_readback=$(sed -n '/TYPE_BEGIN/,/NOINPUT_BEGIN/p' "$TEST_CLEAN_LOG" \
    | grep -c '^DRIVEROUT$' || true)

echo ""
printf "Results: pipe=%d textpipe=%d redir=%d noinput_eof=%d done=%d  (probe: emit_console=%d type_readback=%d)\n" \
    "$pipe" "$textpipe" "$redir" "$noinput" "$done_marker" "$emit_console" "$type_readback"
printf "Self-locate (public install): pipe=%d textpipe=%d redir=%d\n" \
    "$self_pipe" "$self_textpipe" "$self_redir"
printf "Stale-bridge warm read (no UAF): warmpipe=%d\n" "$stale_warm"

# Record the > probe verdict explicitly for the task report.
if [[ "$type_readback" -ge 1 ]]; then
    echo "PROBE(>): out.txt contained DRIVEROUT (read back by 'type') -> '>' redirect CAPTURED driver StdOut. StdOut bridging NOT needed."
elif [[ "$emit_console" -ge 1 ]]; then
    echo "PROBE(>): DRIVEROUT went to the CONSOLE, out.txt empty -> '>' redirect did NOT capture driver StdOut. StdOut bridging needed (Task 4)."
else
    echo "PROBE(>): DRIVEROUT not observed at all -> emit verb did not run as expected (investigate)."
fi

# GREEN requires the three core cases: PIPE, < REDIRECT, no-regression EOF,
# and that the shell reached STDIO_DONE (script ran to completion).
if [[ "$pipe" -ge 1 && "$textpipe" -ge 1 && "$redir" -ge 1 && "$noinput" -ge 1 && "$done_marker" -ge 1 \
      && "$self_pipe" -ge 1 && "$self_textpipe" -ge 1 && "$self_redir" -ge 1 \
      && "$stale_warm" -ge 1 ]]; then
    echo "Driver stdio-bridge test: OK"
    exit 0
else
    echo "Driver stdio-bridge test: FAIL"
    exit 1
fi
