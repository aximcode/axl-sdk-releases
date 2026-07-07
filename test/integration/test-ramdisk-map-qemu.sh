#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# test-ramdisk-map-qemu.sh — mkrd assigns a usable shell map alias via
# EFI_SHELL_PROTOCOL.SetMap, so a non-interactive .nsh can use the RAM disk in
# ONE call with NO `map -r`. A ~28-check matrix over one QEMU boot, exercising
# the mapping/alias behavior AND the other mkrd options:
#   - no-args / -h        -> print help
#   - `mkrd RAMDISK`      -> maps as RAMDISK: (the label), sets %RAMDISK%, usable
#   - `mkrd RAMDISK` again-> idempotent: same alias, "reused", no drift/duplicate
#   - three distinct labels (ALPHA/BETA/GAMMA) coexist, each mapped as itself
#   - `-l` lists them; `-d GAMMA` destroys; `-l` again drops GAMMA
#   - `mkrd fs9`          -> fsN-pattern label REFUSED as alias -> auto fsN + note
#   - `mkrd fs0`          -> never clobbers the boot volume
#   - `mkrd SCRATCH -a RD`-> explicit alias; bare re-run reuses RD (dedup)
#   - label taken by another volume / shell-unsafe / over-long -> fsN fallback
#   - `-a RD` taken / `-a fs5` (reserved, case+':') / over-long `-a` -> rejected
#   - `-v`                -> enables the protocol probe; quiet (no [INFO]) without
# This is the read-only-boot ePSA scratch-volume workflow (delldiags).
#
# Usage: ./test/integration/test-ramdisk-map-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "ERROR: unknown arg '$1'"; exit 2 ;;
    esac
done
case "$ARCH" in
    X64)     NATIVE=x64 ;;
    AARCH64) NATIVE=aa64 ;;
    *) echo "ERROR: --arch X64|AARCH64"; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MKRD="$PROJECT_DIR/out/native-$NATIVE/tools/mkrd.efi"

make -C "$PROJECT_DIR" ARCH="$NATIVE" tools >/dev/null 2>&1 || true
[[ -f "$MKRD" ]] || { echo "ERROR: $MKRD not built"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"
LOG="$TMP/serial.log"
# NB: avoid "-r" / "()" in echo markers — the shell's echo treats a bare
# "-r" token as a flag. Use plain ASCII markers.
cat > "$NSH" <<'EOF'
@echo -off
fs0:
echo MARK_NOARGS
mkrd.efi
echo MARK_HELP
mkrd.efi -h
echo MARK_CLEAN
mkrd.efi RAMDISK -s 4
echo VAR_CLEAN=[%RAMDISK%]
%RAMDISK%:
echo clean-ok > c.txt
type c.txt
fs0:
echo MARK_RERUN
mkrd.efi RAMDISK -s 4
echo VAR_RERUN=[%RAMDISK%]
fs0:
echo MARK_MULTI
mkrd.efi ALPHA -s 4
echo VAR_ALPHA=[%ALPHA%]
mkrd.efi BETA -s 4
echo VAR_BETA=[%BETA%]
mkrd.efi GAMMA -s 4
echo VAR_GAMMA=[%GAMMA%]
ALPHA:
echo alpha-ok > a.txt
type a.txt
BETA:
echo beta-ok > b.txt
type b.txt
fs0:
echo MARK_FSPAT
mkrd.efi fs9 -s 4
echo VAR_FSPAT=[%fs9%]
fs0:
echo MARK_CLOBBER
mkrd.efi fs0 -s 4
echo VAR_CLOBBER=[%fs0%]
fs0:
dir mkrd.efi
echo MARK_NAMED
mkrd.efi SCRATCH -a RD -s 4
echo VAR_NAMED=[%SCRATCH%]
RD:
echo rd-ok > r.txt
type r.txt
fs0:
echo MARK_MIX
mkrd.efi SCRATCH -s 4
echo VAR_MIX=[%SCRATCH%]
echo MARK_LABELTAKEN
mkrd.efi RD -s 4
echo VAR_LABELTAKEN=[%RD%]
echo MARK_TAKEN
mkrd.efi OTHER -a RD
echo MARK_ARESERVED
mkrd.efi ZED -a fs5
echo MARK_ARESERVED2
mkrd.efi ZED2 -a FS5:
echo MARK_UNSAFE
mkrd.efi BADX.Y -s 4
echo MARK_LONG
mkrd.efi THISLABELISTOOLONG -s 4
echo MARK_TOOLONGA
mkrd.efi OTHER2 -a THISNAMEISWAYTOOLONG16
echo MARK_VERBOSE
mkrd.efi -v VTEST -s 4
fs0:
echo MARK_LIST
mkrd.efi -l
echo MARK_DESTROY
mkrd.efi -d GAMMA
echo MARK_LIST2
mkrd.efi -l
echo MARK_UNMAP
GAMMA:
echo MARK_DONE
EOF

timeout=60
[[ "$ARCH" == "AARCH64" || ! -r /dev/kvm ]] && timeout=200
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$timeout" \
    --nsh "$NSH" "$MKRD" > "$LOG" 2>&1 || true

echo "=== markers ==="
sed -n '/MARK_NOARGS/,/MARK_DONE/p' "$LOG" | grep -aE "RAM disk (created|reused)|destroyed|mapping|VAR_|already in use|another volume|reserved fsN|not added|too long|List existing|File Not Found|MARK_" | sed 's/^/  /'

fail=0
# Extract the value from an `echo VAR_X=[value]` line.
val()  { grep -aoE "$1=\[[^]]*\]" "$LOG" | head -1 | sed -E "s/^$1=\[(.*)\]\$/\1/"; }
# The log slice between two markers.
sect() { sed -n "/$1/,/$2/p" "$LOG"; }
ok()   { echo "PASS: $1"; }
no()   { echo "FAIL: $1"; fail=1; }

echo ""
# --- 0. No-args and -h both print help ("List existing RAM disks" is help-only) ---
sect MARK_NOARGS MARK_HELP | grep -aqF "List existing RAM disks" \
    && ok "no-args prints help" || no "no-args did not print help"
sect MARK_HELP MARK_CLEAN | grep -aqF "List existing RAM disks" \
    && ok "-h prints help" || no "-h did not print help"

# --- 1. Clean label becomes the alias verbatim (mkrd RAMDISK -> RAMDISK:) ---
[[ "$(val VAR_CLEAN)" == "RAMDISK" ]] \
    && ok "clean label maps as itself (%RAMDISK% = RAMDISK)" \
    || no "clean label did not map as itself (got '$(val VAR_CLEAN)')"
grep -aqF "clean-ok" "$LOG" && ok "RAMDISK: usable with no map -r" || no "RAMDISK: not usable"

# --- 2. Idempotent re-run: same alias, reported reused, no drift ---
[[ "$(val VAR_RERUN)" == "RAMDISK" && "$(val VAR_CLEAN)" == "RAMDISK" ]] \
    && ok "re-run is idempotent (%RAMDISK% stable = RAMDISK)" \
    || no "re-run drifted ('$(val VAR_CLEAN)' -> '$(val VAR_RERUN)')"
sect MARK_RERUN MARK_MULTI | grep -aqF "reused" \
    && ok "re-run reported reuse, not a fresh map" || no "re-run not reported as reuse"

# --- 3. Multiple distinct clean labels coexist, each mapped as itself ---
[[ "$(val VAR_ALPHA)" == "ALPHA" && "$(val VAR_BETA)" == "BETA" && "$(val VAR_GAMMA)" == "GAMMA" ]] \
    && ok "three distinct labels coexist (ALPHA/BETA/GAMMA each map as themselves)" \
    || no "distinct labels not mapped as themselves (A='$(val VAR_ALPHA)' B='$(val VAR_BETA)' G='$(val VAR_GAMMA)')"
grep -aqF "alpha-ok" "$LOG" && grep -aqF "beta-ok" "$LOG" \
    && ok "coexisting ALPHA: and BETA: both usable" || no "coexisting named volumes not both usable"

# --- 3b. -l lists created disks; -d destroys; -l again drops it ---
if sect MARK_LIST MARK_DESTROY | grep -aqF "ALPHA" \
   && sect MARK_LIST MARK_DESTROY | grep -aqF "BETA" \
   && sect MARK_LIST MARK_DESTROY | grep -aqF "GAMMA"; then
    ok "-l lists the created disks (ALPHA/BETA/GAMMA)"
else
    no "-l did not list all created disks"
fi
sect MARK_DESTROY MARK_LIST2 | grep -aqF "destroyed" \
    && ok "-d GAMMA reports destroyed" || no "-d GAMMA did not report destroyed"
sect MARK_DESTROY MARK_LIST2 | grep -aqF "unmapped" \
    && ok "-d GAMMA unmaps the dangling shell alias" || no "-d GAMMA did not unmap the alias"
if sect MARK_LIST2 MARK_UNMAP | grep -aqF "ALPHA" \
   && ! sect MARK_LIST2 MARK_UNMAP | grep -aqF "GAMMA"; then
    ok "-l after -d: GAMMA gone, ALPHA remains"
else
    no "-l after destroy did not drop GAMMA (or lost ALPHA)"
fi
# the destroyed alias no longer resolves (no dangling map -> no freed-mem deref)
sect MARK_UNMAP MARK_DONE | grep -aqF "not a valid mapping" \
    && ok "GAMMA: no longer resolves after destroy (alias removed)" \
    || no "GAMMA: still resolved after destroy (dangling alias)"

# --- 4. fsN-pattern label is REFUSED as an alias -> auto fsN, with a note ---
fspat="$(val VAR_FSPAT)"
[[ "$fspat" =~ ^fs[0-9]+$ && "$fspat" != "fs9" ]] \
    && ok "fsN-pattern label 'fs9' refused as alias -> auto '$fspat'" \
    || no "fsN-pattern label leaked into the fsN namespace (got '$fspat')"
sect MARK_FSPAT MARK_CLOBBER | grep -aqF "reserved fsN name" \
    && ok "fsN-pattern label explains the fsN fallback" || no "fsN-pattern label fallback not explained"

# --- 5. Label matching the boot volume must NOT clobber it ---
if sect MARK_CLOBBER MARK_NAMED | grep -aqF "mkrd.efi" \
   && ! sect MARK_CLOBBER MARK_NAMED | grep -aqF "File Not Found"; then
    ok "mkrd fs0 did NOT clobber the boot volume (fs0 still readable)"
else
    no "boot volume fs0 was clobbered / unreadable after 'mkrd fs0'"
fi

# --- 6. Explicit -a NAME maps as NAME ---
[[ "$(val VAR_NAMED)" == "RD" ]] && ok "-a RD maps as RD (%SCRATCH% = RD)" \
    || no "-a RD did not map as RD (got '$(val VAR_NAMED)')"
grep -aqF "rd-ok" "$LOG" && ok "RD: usable with no map -r" || no "RD: not usable"

# --- 7. Custom-then-auto dedup: bare re-run reuses the -a alias, no duplicate ---
[[ "$(val VAR_MIX)" == "RD" ]] \
    && ok "bare re-run of a -a-mapped disk reuses its alias (%SCRATCH% = RD)" \
    || no "custom-then-auto re-run drifted (got '$(val VAR_MIX)', want RD)"
sect MARK_MIX MARK_LABELTAKEN | grep -aqF "reused" \
    && ok "custom-then-auto re-run reported reuse" || no "custom-then-auto not reported as reuse"

# --- 8. Label already taken by ANOTHER volume -> fallback fsN, no clobber ---
sect MARK_LABELTAKEN MARK_TAKEN | grep -aqF "already in use by another volume" \
    && ok "label taken by another volume falls back (no clobber)" \
    || no "label-collision fallback note missing"
lt="$(val VAR_LABELTAKEN)"
[[ "$lt" =~ ^fs[0-9]+$ ]] && ok "label-collision disk got an fsN ('$lt')" \
    || no "label-collision disk alias unexpected (got '$lt')"

# --- 9. -a NAME already in use is refused (strict pin) ---
grep -aq 'shell map alias "RD" is already in use' "$LOG" \
    && ok "-a RD refused when RD: already mapped" || no "taken -a name not refused"

# --- 9b. Explicit -a fsN (reserved) is a HARD error, case- and ':'-insensitive ---
sect MARK_ARESERVED MARK_ARESERVED2 | grep -aqF "reserved fsN name" \
    && ok "-a fs5 (reserved) rejected" || no "-a fs5 not rejected"
sect MARK_ARESERVED2 MARK_UNSAFE | grep -aqF "reserved fsN name" \
    && ok "-a FS5: (reserved, case+colon) rejected" || no "-a FS5: not rejected"

# --- 10. Shell-unsafe label -> fallback fsN (not mapped verbatim) ---
sect MARK_UNSAFE MARK_LONG | grep -aqE "mapping : fs[0-9]+:" \
    && ok "shell-unsafe label 'BADX.Y' falls back to fsN" \
    || no "unsafe label was not mapped to fsN"

# --- 11. Over-long label -> fallback fsN ---
sect MARK_LONG MARK_TOOLONGA | grep -aqE "mapping : fs[0-9]+:" \
    && ok "over-long label falls back to fsN" || no "over-long label not mapped to fsN"

# --- 12. Over-long -a name rejected ---
grep -aq 'is too long' "$LOG" && ok "over-long -a name rejected" || no "over-long -a not rejected"

# --- 13. Quiet by default: no [INFO]/[DEBUG] before the -v run; -v enables it ---
if sed -n '1,/MARK_VERBOSE/p' "$LOG" | grep -aqE '\[(INFO|DEBUG)\]'; then
    no "library log chatter leaked without -v"; sed -n '1,/MARK_VERBOSE/p' "$LOG" | grep -aE '\[(INFO|DEBUG)\]' | head -3 | sed 's/^/    /'
else
    ok "no [INFO]/[DEBUG] chatter without -v"
fi
sect MARK_VERBOSE MARK_DONE | grep -aqF "PROBE" \
    && ok "-v enables diagnostics (protocol probe shown)" || no "-v did not enable diagnostics"

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): mkrd label-default mapping + guards verified ==="; exit 0
else
    echo "=== FAIL ($ARCH) ==="; exit 1
fi
