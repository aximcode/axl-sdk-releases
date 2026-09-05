#!/bin/bash
# test-meta: arch=x64 needs= est=25 local-only=0
# test-host-toolchain-coverage.sh — EVERY shipped example and tool must LINK
# under AXL_TOOLCHAIN=host.
#
# Compile-only would not do. The spike's second wrong answer -- "no libc is
# needed at all under host" -- was concluded from four simple sources that all
# linked fine. It was wrong: libaxl.a's VENDORED code (libvterm, lzma,
# mbedTLS) references the plain C leaf names (memcpy, memcmp, memmove, memset,
# strchr, strlen), and --gc-sections drops those references whenever the
# vendored code is unused -- so a simple source links with no libc at all and
# proves nothing. sdk/examples/http-server.c is the shipped source that KEEPS
# them, and it is what exposed the error. See
# docs/AXL-Host-Toolchain-Design.md §7.2. `make check-examples` is
# compile-only and would not have caught it; this gate links.
#
# Sources: sdk/examples/*.c and tools/*.c -- the SOURCE trees, not a staged
# path. scripts/install.sh stages no examples at all; share/doc/axl-sdk/
# examples/ exists only inside the release TARBALL.
#
# *.c ONLY, never *.cpp: C++ is refused under host BY DESIGN (spec §6.3), so a
# .cpp in this loop would assert the opposite of the spec.
#
# Most sources link with one plain `axl-cc src.c -o out.efi` call. A minority
# need the SAME flags a real consumer would pass -- --type driver,
# --allow-uefi, --embed, --service -- because they ARE drivers, raw-UEFI
# apps, or embed a companion blob (a launcher, a service pair). Which flag
# each needs is not a guess: the driver/app split comes from reading each
# file for an actual DriverEntry (AXL_DRIVER / AXL_SERVICE_DRIVER), and the
# raw-UEFI split reuses scripts/check-uefi-scope.py's ALLOWED dict -- the
# authoritative, already-reviewed record of which shipped tools/examples
# touch UEFI and why. A handful of sources have NO axl-cc build shape at all
# (a busybox multiplexer, pure support modules with no entry point, one
# deliberately-bypasses-axl-cc spike) and are reported as an explicit, named
# SKIP -- never silently dropped from the loop. See the case statement below
# for the reasoning behind each entry.

source "$(dirname "$0")/common-test.sh"
# Every source below is expected to either link or fail for a KNOWN, named
# reason; -e (inherited from common-test.sh) would abort the whole run at the
# first nonzero exit instead of recording it and moving on.
set +e
TEST_SKIP_RATCHET=1

SDK="$(test_sdk_dir)"
AXL_CC="$SDK/bin/axl-cc"
if [[ ! -x "$AXL_CC" || ! -d "$SDK/lib/axl/x64" ]]; then
    echo "ERROR: staged SDK missing at $SDK — run 'scripts/install.sh --arch x64' first" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# host mode is explicit, not left to `auto`: on a box with the bare-metal
# toolchain installed (this one), an unset/auto AXL_TOOLCHAIN resolves to
# `axl` and this gate would silently stop testing the variant it exists for.
export AXL_TOOLCHAIN=host

check() {  # check <rc:0=pass> <msg>
    if [[ "$1" == "0" ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
}

# run_link LOG MSG CMD... — run CMD, tally the result, and on failure dump a
# few lines of its log so a real gap (an undefined libc symbol) is visible in
# the run's own output, not just in a discarded temp file. Every call is one
# source that REACHED the linker, so this is where n_linked is counted (see
# MIN_LINKED below); the arms that do not go through run_link count their own.
run_link() {
    local log="$1" msg="$2"; shift 2
    n_linked=$((n_linked + 1))
    "$@" > "$log" 2>&1
    local rc=$?
    check "$rc" "$msg"
    [[ $rc -ne 0 ]] && tail -8 "$log" | sed 's/^/      /'
    return $rc
}

echo "=== AXL_TOOLCHAIN=host: every sdk/examples/ and tools/ source LINKS ==="

# Assert the variant actually in force, rather than assuming the export above
# took. axl-cc --print-toolchain is the resolver's own answer (the contract is
# the bare variant word on stdout; `axl toolchain list` asks the same way), so
# this pins WHAT this run exercised instead of leaving it to circumstance --
# on a box with the bare-metal toolchain installed, a resolution to `axl`
# would still link all 90 sources and print a green table for the wrong
# variant. Fatal, not a tallied FAIL among 90 passes: every "host links:" line
# below would be a lie, so there is nothing worth measuring afterwards.
resolved="$("$AXL_CC" --print-toolchain 2>&1)"
if [[ "$resolved" != "host" ]]; then
    check 1 "resolved toolchain is 'host' (axl-cc --print-toolchain said '$resolved')"
    test_host_summary "host-toolchain-coverage"
    exit 1
fi
check 0 "resolved toolchain is 'host' (axl-cc --print-toolchain)"

# The floor: a glob that matches nothing must FAIL, not report clean forever
# (the same reason make check-examples carries CHECK_EX_MIN). 90 is a safety
# margin below the 96 sources actually found as of 2026-09-03 (51
# sdk/examples/*.c + 46 tools/*.c, minus fwtool.c) -- the same generous-margin
# shape as check-examples' CHECK_EX_MIN (20 against 51 real examples).
MIN_SOURCES=90

# The SECOND floor, on links ACTUALLY PERFORMED. MIN_SOURCES guards the
# ITERATION count only, and the named SKIPs below are hard-coded `check 0`
# passes -- so a growing SKIP list could hollow this gate out to a handful of
# real links while every counter it checks stayed green. n_linked counts the
# sources that reached the linker: 90 non-SKIP arms plus the four 9p support
# modules that link inside the 9p.c invocation = 94 as of 2026-09-03. 88 is
# the same generous-margin shape as MIN_SOURCES.
MIN_LINKED=88

n=0
n_linked=0
for src in "$PROJECT_DIR"/sdk/examples/*.c "$PROJECT_DIR"/tools/*.c; do
    [ -e "$src" ] || continue
    # fwtool's hosted-libc includes are behind #ifdef AXL_HOSTED -- the
    # host-SIDE build, not the UEFI one this gate covers.
    case "$src" in *fwtool.c) continue ;; esac
    n=$((n + 1))
    base="$(basename "$src")"
    dir="$(dirname "$src")"
    out="$WORK/${base}.efi"
    log="$WORK/${base}.log"

    case "$base" in

    # -----------------------------------------------------------------
    # No axl-cc build shape exists for these AT ALL -- a link-SHAPE
    # mismatch, not a libc gap. Named explicitly so the exclusion is
    # visible and auditable rather than a silent hole in the loop.
    # -----------------------------------------------------------------
    axl.c)
        # The busybox multiplexer: needs -DAXL_BUSYBOX (include/axl.h's
        # AXL_TOOL_MAIN renames every OTHER tool's main() to
        # axl_tool_<name>_main under that flag, per AXL_TOOL_ENTRY_) PLUS
        # every one of the ~30 TOOL_NAMES objects linked together. Each of
        # those siblings is ALREADY linked standalone elsewhere in this same
        # loop, so replicating the multiplexer here would not exercise any
        # libc surface this run does not already cover -- only axl.c's own
        # dispatch glue, which touches no vendored code.
        check 0 "host: SKIP $base — busybox multiplexer (-DAXL_BUSYBOX + every TOOL_NAMES object); each sibling tool links standalone elsewhere in this run"
        continue
        ;;
    hello-minimal.c)
        # Deliberately bypasses axl-cc and libaxl entirely -- its own
        # docstring calls it "NOT the shape a normal consumer wants". It
        # hand-declares its own _AxlEntry (a different UEFI ABI) and links
        # with raw ld against $(GCC_CRT0)+$(RELOC_OBJ) alone, never
        # libaxl.a. "make hello-minimal" is its only build path; there is no
        # axl-cc invocation, bare-metal OR host, that produces it.
        check 0 "host: SKIP $base — bypasses axl-cc/libaxl by design (own _AxlEntry, no libaxl.a); built only via 'make hello-minimal'"
        continue
        ;;
    9p-common.c|9p-cmd-file.c|9p-cmd-serve.c|9p-cmd-mount.c)
        # Pure support modules for the 9p.efi launcher: none of the four
        # ever defines main() or DriverEntry, with or without any -D flag
        # (confirmed by reading each file — no #else arm exists for their
        # #ifdef blocks). They are exercised as part of the 9p.c multi-file
        # link below, never standalone.
        check 0 "host: SKIP $base — no standalone entry point; linked only as part of the 9p.c launcher build below"
        continue
        ;;

    # -----------------------------------------------------------------
    # Drivers: DriverEntry via AXL_DRIVER, or a hand-written DriverEntry.
    # --type driver supplies the right entry point + subsystem AND
    # auto-grants AXL_ALLOW_UEFI to non-app images (axl-cc's own policy:
    # "every NON-APP image type is granted it unconditionally").
    # -----------------------------------------------------------------
    binding-driver.c|driver.c|http-server-driver.c|reload-svc-dxe.c|\
    reload-svc-fail-dxe.c|smbus-hc-shim.c|fbcon-drv.c|kbtune-drv.c)
        run_link "$log" "host links: $base (--type driver)" \
            "$AXL_CC" --type driver "$src" -o "$out"
        ;;

    # AXL_SERVICE_DRIVER(...) here is itself wrapped in
    # #ifdef AXL_SERVICE_BUILD_DRIVER (the Makefile's own dual-compile flag
    # for this pattern); without the define the file compiles to an empty
    # stub with no entry point at all.
    9p-serve-svc.c|9p-mount-svc.c)
        run_link "$log" "host links: $base (--type driver -DAXL_SERVICE_BUILD_DRIVER)" \
            "$AXL_CC" --type driver -DAXL_SERVICE_BUILD_DRIVER "$src" -o "$out"
        ;;

    # -----------------------------------------------------------------
    # App-type sources that touch raw UEFI directly. Declared in
    # scripts/check-uefi-scope.py's ALLOWED dict (the authoritative,
    # already-reviewed record of which shipped tools/examples touch UEFI and
    # why) -- --allow-uefi is axl-cc's sanctioned opt-in for an application,
    # matching how a real consumer would build these.
    # -----------------------------------------------------------------
    pointer-tune-demo.c|crashtest.c|mkfixture.c|netload.c)
        run_link "$log" "host links: $base (--allow-uefi)" \
            "$AXL_CC" --allow-uefi "$src" -o "$out"
        ;;

    mkrd.c)
        # Embeds the vendored RamDiskDxe.efi (third_party/edk2) as a
        # fallback for firmware without EFI_RAM_DISK_PROTOCOL -- the same
        # blob the Makefile's EMBED_BLOB macro embeds in-tree.
        run_link "$log" "host links: $base (--allow-uefi --embed ramdiskdxe)" \
            "$AXL_CC" --allow-uefi \
            --embed "$PROJECT_DIR/third_party/edk2/RamDiskDxe-x64.efi=ramdiskdxe" \
            "$src" -o "$out"
        ;;

    fbcon.c)
        # The launcher embeds its own resident driver (fbcon-drv.efi).
        # Built fresh right here rather than assumed already produced by
        # tools/fbcon-drv.c's own loop iteration: bash's glob order is
        # LC_COLLATE-dependent (verified: under this box's locale "fbcon.c"
        # can sort before "fbcon-drv.c", the opposite of plain ASCII), so a
        # cross-iteration dependency on visitation order is not safe to
        # assume. Self-contained also matches how a real consumer would
        # actually do this -- two separate axl-cc invocations, exactly like
        # the Makefile's own two-step recipe.
        drv="$WORK/_prereq-fbcon-drv.efi"
        "$AXL_CC" --type driver "$dir/fbcon-drv.c" -o "$drv" \
            > "$WORK/_prereq-fbcon-drv.log" 2>&1
        if [[ $? -ne 0 ]]; then
            check 1 "host links: $base (--allow-uefi --embed fbcon-drv; the prerequisite driver build failed)"
            tail -8 "$WORK/_prereq-fbcon-drv.log" | sed 's/^/      /'
        else
            run_link "$log" "host links: $base (--allow-uefi --embed fbcon-drv)" \
                "$AXL_CC" --allow-uefi --embed "$drv=fbcon_drv" "$src" -o "$out"
        fi
        ;;

    # -----------------------------------------------------------------
    # Non-driver embed: an arbitrary companion data file, per the doc
    # comment at the top of embed-asset.c itself.
    # -----------------------------------------------------------------
    embed-asset.c)
        run_link "$log" "host links: $base (--embed greeting)" \
            "$AXL_CC" --embed "$dir/embed-asset.txt=greeting" "$src" -o "$out"
        ;;

    # -----------------------------------------------------------------
    # AXL_SERVICE(svc) / a hand-written equivalent (each file has its own
    # #ifdef AXL_SERVICE_BUILD_DRIVER split). The documented consumer recipe
    # is `axl-cc --service NAME src.c`, which dual-compiles into a driver
    # image AND a launcher app embedding it -- two real links per source,
    # both asserted here.
    # -----------------------------------------------------------------
    service-demo.c|svc-startfail.c|svc-embonly.c|service-demo-custom.c)
        case "$base" in
            service-demo.c)        svc=service_demo ;;
            svc-startfail.c)       svc=svc_startfail ;;
            svc-embonly.c)         svc=svc_embonly ;;
            service-demo-custom.c) svc=service_demo_custom ;;
        esac
        # Two real links (driver image + launcher) from one invocation; the
        # counter is per SOURCE that reached the linker, so it counts 1.
        n_linked=$((n_linked + 1))
        ( cd "$WORK" && "$AXL_CC" --service "$svc" "$src" ) > "$log" 2>&1
        rc=$?
        if [[ $rc -eq 0 && -f "$WORK/$svc.efi" && -f "$WORK/$svc-dxe.efi" ]]; then
            check 0 "host links: $base (--service $svc: launcher + driver)"
        else
            check 1 "host links: $base (--service $svc: launcher + driver)"
            tail -8 "$log" | sed 's/^/      /'
        fi
        ;;

    # -----------------------------------------------------------------
    # The 9p launcher: a genuine multi-file link (5 sources) plus its two
    # embedded driver images. The two drivers are built fresh right here
    # (see the fbcon.c case above for why a cross-iteration dependency on
    # glob visitation order is not safe to assume) rather than reused from
    # 9p-serve-svc.c / 9p-mount-svc.c's own loop iterations.
    # -----------------------------------------------------------------
    9p.c)
        drv_serve="$WORK/_prereq-9p-serve-dxe.efi"
        drv_mount="$WORK/_prereq-9p-mount-dxe.efi"
        "$AXL_CC" --type driver -DAXL_SERVICE_BUILD_DRIVER \
            "$dir/9p-serve-svc.c" -o "$drv_serve" \
            > "$WORK/_prereq-9p-serve-dxe.log" 2>&1
        serve_rc=$?
        "$AXL_CC" --type driver -DAXL_SERVICE_BUILD_DRIVER \
            "$dir/9p-mount-svc.c" -o "$drv_mount" \
            > "$WORK/_prereq-9p-mount-dxe.log" 2>&1
        mount_rc=$?
        if [[ $serve_rc -ne 0 || $mount_rc -ne 0 ]]; then
            check 1 "host links: $base (multi-file launcher; a prerequisite driver build failed)"
            tail -8 "$WORK/_prereq-9p-serve-dxe.log" | sed 's/^/      /'
            tail -8 "$WORK/_prereq-9p-mount-dxe.log" | sed 's/^/      /'
        else
            # 9p-common.c and the three 9p-cmd-*.c modules are SKIPped as
            # standalone sources above precisely because they reach the linker
            # only here -- so this is where they count.
            n_linked=$((n_linked + 4))
            run_link "$log" \
                "host links: $base (multi-file launcher: 9p.c+9p-common.c+9p-cmd-{file,serve,mount}.c+9p-{serve,mount}-svc.c, 2 embedded driver blobs)" \
                "$AXL_CC" \
                --embed "$drv_serve=serve9p_dxe" \
                --embed "$drv_mount=mount9p_dxe" \
                "$src" "$dir/9p-common.c" "$dir/9p-cmd-file.c" \
                "$dir/9p-cmd-serve.c" "$dir/9p-cmd-mount.c" \
                "$dir/9p-serve-svc.c" "$dir/9p-mount-svc.c" \
                -o "$out"
        fi
        ;;

    # -----------------------------------------------------------------
    # http-server.c specifically: identical to the wildcard build below, but
    # the output path is stashed for the host-binutils PE/reloc inspection
    # after the loop (see there for why this source and not another).
    # -----------------------------------------------------------------
    http-server.c)
        run_link "$log" "host links: $base" "$AXL_CC" "$src" -o "$out"
        HTTP_SERVER_HOST_EFI="$out"
        ;;

    # -----------------------------------------------------------------
    # Everything else: a plain app that needs nothing special.
    # -----------------------------------------------------------------
    *)
        run_link "$log" "host links: $base" "$AXL_CC" "$src" -o "$out"
        ;;
    esac
done

[ "$n" -ge "$MIN_SOURCES" ]
check $? "host links: found $n sources, at least $MIN_SOURCES expected"

[ "$n_linked" -ge "$MIN_LINKED" ]
check $? "host links: $n_linked sources reached the linker, at least $MIN_LINKED expected"

echo
echo "=== AXL_TOOLCHAIN=host: the host binutils' own PE/reloc output, inspected ==="
# NO GATE EVER INSPECTS A HOST-PRODUCED IMAGE otherwise. check-nx-compat,
# check-no-avx, check-reloc-coverage and check-bss-clear (Makefile) all run
# over $(PREFIX)/*.efi built by the SDK's own BARE-METAL binutils -- the only
# host-produced image ever EXECUTED anywhere is hello.efi on OVMF, the
# smallest workload in the tree. check-reloc-coverage exists because a
# DT_RELA table split across two sections, or dropped by objcopy -j's
# exact-name list, was a latent aa64 bug for years, and section naming is a
# binutils-VERSION property -- exactly where the HOST's binutils, a
# different binary from a different vendor on every machine this runs on,
# could differ. Reuses http-server.c.efi from the loop above (the source
# that keeps enough vendored code to need the standin archive, see the file
# header) rather than a second build.
if [[ -n "${HTTP_SERVER_HOST_EFI:-}" && -f "$HTTP_SERVER_HOST_EFI" ]]; then
    python3 "$PROJECT_DIR/scripts/check-pe-nx.py" "$HTTP_SERVER_HOST_EFI" \
        > "$WORK/_host-nx.log" 2>&1
    rc=$?
    check "$rc" "host binutils: NX_COMPAT set on host-produced http-server.efi"
    [[ $rc -ne 0 ]] && tail -8 "$WORK/_host-nx.log" | sed 's/^/      /'

    # DIRECT, not inferred: check-reloc-coverage.py's own check() returns a
    # clean PASS ([]) for a fully-static image with NO DT_RELA table at all --
    # legitimate for a genuinely static binary, but it means the "table
    # intact" PASS just below is vacuous unless THIS image actually carries
    # one. Reuses the checker's own objdump_for()/dynamic_rela() rather than a
    # second regex that could drift from what the checker itself parses, so
    # this is asserting the SAME fact the checker's invariant 1 depends on,
    # not a parallel guess at it.
    python3 - "$PROJECT_DIR/scripts/check-reloc-coverage.py" \
        "${HTTP_SERVER_HOST_EFI%.efi}.so" > "$WORK/_host-reloc-present.log" 2>&1 <<'PYEOF'
import importlib.util, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location("crc", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
so = Path(sys.argv[2])
tool = mod.objdump_for(so)
rela = mod.dynamic_rela(tool, so)
if rela is None or rela[1] == 0:
    print(f"NO usable DT_RELA table: {rela!r}")
    sys.exit(1)
print(f"DT_RELA={rela[0]:#x} DT_RELASZ={rela[1]:#x}")
PYEOF
    rc=$?
    check "$rc" "host binutils: http-server.so genuinely carries a non-empty DT_RELA table (so the PASS below is not vacuous)"
    [[ $rc -ne 0 ]] && tail -8 "$WORK/_host-reloc-present.log" | sed 's/^/      /'

    python3 "$PROJECT_DIR/scripts/check-reloc-coverage.py" "$HTTP_SERVER_HOST_EFI" \
        > "$WORK/_host-reloc.log" 2>&1
    rc=$?
    check "$rc" "host binutils: DT_RELA table intact in host-produced http-server.efi"
    [[ $rc -ne 0 ]] && tail -8 "$WORK/_host-reloc.log" | sed 's/^/      /'

    # CONTROL, both checks. A gate that has never failed is unproven -- prove
    # each one actually SEES the defect it exists for, on a deliberately-
    # broken COPY of THIS run's own host-built image, so the control uses
    # this box's own host binutils rather than a canned fixture.
    _ctrl_nx="$WORK/_ctrl-nonx.efi"
    python3 - "$HTTP_SERVER_HOST_EFI" "$_ctrl_nx" <<'PYEOF'
import struct, sys
data = bytearray(open(sys.argv[1], "rb").read())
pe_off = struct.unpack_from("<I", data, 60)[0]
off = pe_off + 4 + 20 + 70          # PE sig(4) + COFF(20) + DllCharacteristics
val = struct.unpack_from("<H", data, off)[0] & ~0x0100
struct.pack_into("<H", data, off, val)
open(sys.argv[2], "wb").write(data)
PYEOF
    python3 "$PROJECT_DIR/scripts/check-pe-nx.py" "$_ctrl_nx" \
        > "$WORK/_host-nx-ctrl.log" 2>&1
    rc=$?
    if [[ $rc -ne 0 ]] && grep -q 'MISSING NX_COMPAT' "$WORK/_host-nx-ctrl.log"; then
        test_host_pass "control: check-pe-nx.py FAILS BY NAME on a copy with NX_COMPAT cleared by hand"
    else
        test_host_fail "control: check-pe-nx.py did not fail by name on a deliberately NX-stripped copy"
        sed 's/^/      /' "$WORK/_host-nx-ctrl.log"
    fi

    # Reproduces the EXACT historical bug (see the file header of
    # check-reloc-coverage.py): re-run objcopy against the same .so, with the
    # SAME -j list axl-cc:2314-2317 actually carries, minus ONLY -j .rela and
    # -j .rela.dyn -- the real host objcopy on this machine, not a hand-edited
    # byte. -j .rel stays in the list (x86_64 ELF has no .rel section, so it
    # is a no-op either way, but the point of a control is to change exactly
    # one thing).
    _ctrl_so="${HTTP_SERVER_HOST_EFI%.efi}.so"
    _ctrl_reloc_efi="$WORK/_ctrl-noreloc.efi"
    objcopy \
        -j .text -j .sdata -j .data -j .bss -j .dynamic -j .dynsym \
        -j .rel -j .reloc -j .rodata -j .dbgdir -j .eh_frame -j .gcc_except_table \
        --strip-all --output-target=pei-x86-64 --subsystem=10 \
        "$_ctrl_so" "$_ctrl_reloc_efi" > "$WORK/_host-reloc-ctrl-objcopy.log" 2>&1
    cp "$_ctrl_so" "${_ctrl_reloc_efi%.efi}.so"
    python3 "$PROJECT_DIR/scripts/check-reloc-coverage.py" "$_ctrl_reloc_efi" \
        > "$WORK/_host-reloc-ctrl.log" 2>&1
    rc=$?
    if [[ $rc -ne 0 ]] && grep -q 'did not survive objcopy' "$WORK/_host-reloc-ctrl.log"; then
        test_host_pass "control: check-reloc-coverage.py FAILS BY NAME when objcopy -j drops .rela (the exact historical bug)"
    else
        test_host_fail "control: check-reloc-coverage.py did not fail by name on a copy missing -j .rela"
        sed 's/^/      /' "$WORK/_host-reloc-ctrl.log"
    fi
else
    test_host_fail "host binutils PE/reloc checks: no host-built http-server.efi to inspect"
fi

test_host_summary "host-toolchain-coverage"
