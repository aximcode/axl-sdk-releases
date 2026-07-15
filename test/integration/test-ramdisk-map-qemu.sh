#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# test-ramdisk-map-qemu.sh — mkrd maps a freshly created RAM disk so it shows in
# a bare `map` IMMEDIATELY (no `map -r`), looking like a native volume:
#
#       FS1: Alias(s):DEMO:
#           VirtualDisk(...)
#
# It does this with TWO EFI_SHELL_PROTOCOL.SetMap calls on the disk's device
# path: the next-free `FS<n>` name FIRST (so the shell's name filter shows it and
# it becomes the PRIMARY column) and the volume LABEL second (so the label lands
# in the `Alias(s):` column). Both `FS<n>:` and `LABEL:` are usable paths with no
# `map -r`. There is NO `-a/--alias` option and NO `%<label>%` env var anymore —
# the label IS the friendly alias, and the fsN is the native handle.
#
# A ~20-check matrix over one QEMU boot:
#   - no-args / -h        -> print help; help no longer advertises -a
#   - `mkrd DEMO`         -> `map` shows `FS<n>: Alias(s):DEMO:`; DEMO: usable
#   - `mkrd DEMO` again   -> idempotent: same fsN + alias, "reused", no duplicate
#   - ALPHA/BETA/GAMMA    -> coexist, each `FS<n>: Alias(s):<LABEL>:`, distinct fsN
#   - `-l` columns MAPPING(fsN)/LABEL/ALIAS(label:)/SIZE/FS; `-d GAMMA` destroys+unmaps
#   - label unusable as an alias (fs-pattern `fs9` / unsafe `BADX.Y` / over-long)
#     -> mapped as `FS<n>:` with NO label alias, and a note says why
#   - `mkrd fs0`          -> never clobbers the boot volume
#   - `mkrd X -a RD`      -> `-a` is gone: "unknown flag -a", no disk created
#   - `-v`                -> protocol probe; quiet (no [INFO]) without it
# This is the read-only-boot scratch-volume workflow (delldiags).
#
# Usage: ./test/integration/test-ramdisk-map-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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
echo MARK_CREATE
mkrd.efi DEMO -s 4
echo MARK_MAP_CREATE
map
echo MARK_USE
DEMO:
echo demo-ok > d.txt
type d.txt
fs0:
echo MARK_USE_FS0
dir d.txt
echo MARK_RERUN
mkrd.efi DEMO -s 4
echo MARK_MAP_RERUN
map
fs0:
echo MARK_MULTI
mkrd.efi ALPHA -s 4
mkrd.efi BETA -s 4
mkrd.efi GAMMA -s 4
echo MARK_MAP_MULTI
map
echo MARK_MULTI_USE
ALPHA:
echo alpha-ok > a.txt
type a.txt
BETA:
echo beta-ok > b.txt
type b.txt
fs0:
echo MARK_FSPAT
mkrd.efi fs9 -s 4
echo MARK_UNSAFE
mkrd.efi BADX.Y -s 4
echo MARK_LONG
mkrd.efi THISLABELISTOOLONG -s 4
echo MARK_CLOBBER
mkrd.efi fs0 -s 4
fs0:
dir mkrd.efi
echo MARK_COLLIDE
map COLL fs0:
mkrd.efi COLL -s 4
COLL:
dir mkrd.efi
fs0:
echo MARK_NOOPT
mkrd.efi NOPT -a RD -s 4
echo MARK_VERBOSE
mkrd.efi -v VTEST -s 4
fs0:
echo MARK_LIST
mkrd.efi -l
echo MARK_DESTROY
mkrd.efi -d GAMMA
echo MARK_LIST2
mkrd.efi -l
echo MARK_QDEAD
map GAMMA:
echo MARK_DONE
reset -s
EOF

timeout=60
[[ "$ARCH" == "AARCH64" || ! -r /dev/kvm ]] && timeout=200
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$timeout" \
    --nsh "$NSH" "$MKRD" > "$LOG" 2>&1 || true

echo "=== markers ==="
sed -n '/MARK_NOARGS/,/MARK_DONE/p' "$LOG" | grep -aE "RAM disk (created|reused)|destroyed|mapping|Alias\(s\)|not a usable shell alias|unknown flag|List existing|File Not Found|MARK_" | sed 's/^/  /'

fail=0
# The log slice between two markers.
sect() { sed -n "/$1/,/$2/p" "$LOG"; }
ok()   { echo "PASS: $1"; }
no()   { echo "FAIL: $1"; fail=1; }

echo ""
# --- 0. No-args and -h both print help; help no longer advertises -a ---
sect MARK_NOARGS MARK_HELP | grep -aqF "List existing RAM disks" \
    && ok "no-args prints help" || no "no-args did not print help"
sect MARK_HELP MARK_CREATE | grep -aqF "List existing RAM disks" \
    && ok "-h prints help" || no "-h did not print help"
sect MARK_HELP MARK_CREATE | grep -aqiF "alias override" \
    && no "-h still advertises the removed -a/--alias option" \
    || ok "-h no longer advertises -a/--alias"

# --- 1. Create maps as `FS<n>: (alias DEMO:)`; no %VAR%, no 'var' line ---
sect MARK_CREATE MARK_MAP_CREATE | grep -aqE "mapping : FS[0-9]+: \(alias DEMO:\)" \
    && ok "create prints 'mapping : FS<n>: (alias DEMO:)'" \
    || no "create did not print the fsN-primary + label-alias mapping line"
if sect MARK_CREATE MARK_MAP_CREATE | grep -aqE "^  var |%DEMO%"; then
    no "create still emits a %VAR% (the '  var :' line / %DEMO%)"
else
    ok "create emits no %<label>% env var"
fi
# The internal `map -r` (axl_map_refresh) must not dump the shell's mapping
# table during create — mkrd prints its own summary. Only the EXPLICIT `map`
# after MARK_MAP_CREATE should. On the modern shell EFI_SHELL_PROTOCOL.Execute
# was already silent (nested shell, off-console), so this is a regression guard;
# the visible leak was the old shell's in-context SHELL_ENVIRONMENT.Execute,
# pinned in test-old-shell-qemu.sh. "Mapping table" is the EDK2 `map` header.
if sect MARK_CREATE MARK_MAP_CREATE | grep -aqiF "Mapping table"; then
    no "internal map -r leaked the mapping table into create output"
else
    ok "internal map -r stays silent during create (no mapping table)"
fi

# --- 2. CORE: bare `map` shows the disk as `FS<n>: Alias(s):DEMO:` (no map -r) ---
sect MARK_MAP_CREATE MARK_USE | grep -aqE "FS[0-9]+: Alias\(s\):DEMO:" \
    && ok "bare map shows disk as 'FS<n>: Alias(s):DEMO:' (native, no map -r)" \
    || no "bare map did not show the disk as fsN-primary with DEMO alias"

# --- 3. DEMO: is a usable, DISTINCT volume with no map -r ---
# demo-ok round-trips on DEMO:, AND d.txt is absent on fs0 afterward — proving
# DEMO: is a real separate volume, not a silent fall-through to the (writable)
# boot fs0 (which would let a dead alias pass a bare whole-log "demo-ok" grep).
grep -aqF "demo-ok" "$LOG" && ok "DEMO: write/read round-trips" || no "DEMO: not usable"
sect MARK_USE_FS0 MARK_RERUN | grep -aqF "File Not Found" \
    && ok "DEMO: is a distinct volume (d.txt not on fs0)" \
    || no "DEMO: may have fallen through to fs0 (d.txt found there)"

# --- 4. Idempotent re-run: reused, exactly one DEMO alias in the map (no dup) ---
sect MARK_RERUN MARK_MAP_RERUN | grep -aqF "reused" \
    && ok "re-run reported reuse, not a fresh map" || no "re-run not reported as reuse"
dupes=$(sect MARK_MAP_RERUN MARK_MULTI | grep -acE "Alias\(s\):DEMO:" || true)
[[ "$dupes" -eq 1 ]] \
    && ok "re-run leaves exactly one DEMO mapping (no duplicate/drift)" \
    || no "re-run produced $dupes DEMO mappings (want 1)"

# --- 5. Three distinct labels coexist, each mapped fsN-primary + own alias ---
for lbl in ALPHA BETA GAMMA; do
    sect MARK_MAP_MULTI MARK_MULTI_USE | grep -aqE "FS[0-9]+: Alias\(s\):$lbl:" \
        && ok "map shows $lbl as 'FS<n>: Alias(s):$lbl:'" \
        || no "map missing fsN-primary + $lbl alias"
done
grep -aqF "alpha-ok" "$LOG" && grep -aqF "beta-ok" "$LOG" \
    && ok "coexisting ALPHA: and BETA: both usable" || no "coexisting named volumes not both usable"

# --- 6. Label unusable as an alias -> mapped as FS<n>: only, WITH a note ---
# fs-pattern label (fs9): reserved, cannot be the alias.
if sect MARK_FSPAT MARK_UNSAFE | grep -aqE "mapping : FS[0-9]+:[[:space:]]*$" \
   && sect MARK_FSPAT MARK_UNSAFE | grep -aqF "not a usable shell alias"; then
    ok "fs-pattern label 'fs9' -> FS<n>: only (no alias), with a note"
else
    no "fs-pattern label did not fall back to a bare fsN mapping + note"
fi
# shell-unsafe label (BADX.Y): '.' is not a clean map token.
if sect MARK_UNSAFE MARK_LONG | grep -aqE "mapping : FS[0-9]+:[[:space:]]*$" \
   && sect MARK_UNSAFE MARK_LONG | grep -aqF "not a usable shell alias"; then
    ok "shell-unsafe label 'BADX.Y' -> FS<n>: only (no alias), with a note"
else
    no "shell-unsafe label did not fall back to a bare fsN mapping + note"
fi
# over-long label: too long to be a map name.
if sect MARK_LONG MARK_CLOBBER | grep -aqE "mapping : FS[0-9]+:[[:space:]]*$" \
   && sect MARK_LONG MARK_CLOBBER | grep -aqF "not a usable shell alias"; then
    ok "over-long label -> FS<n>: only (no alias), with a note"
else
    no "over-long label did not fall back to a bare fsN mapping + note"
fi

# --- 7. Label matching the boot volume must NOT clobber it ---
if sect MARK_CLOBBER MARK_COLLIDE | grep -aqF "mkrd.efi" \
   && ! sect MARK_CLOBBER MARK_COLLIDE | grep -aqF "File Not Found"; then
    ok "mkrd fs0 did NOT clobber the boot volume (fs0 still readable)"
else
    no "boot volume fs0 was clobbered / unreadable after 'mkrd fs0'"
fi

# --- 7b. A label colliding with ANOTHER volume's map name must not repoint it ---
# COLL is manually mapped to fs0, then `mkrd COLL` must NOT hijack COLL: (SetMap
# would otherwise silently repoint it at the RAM disk). The alias is skipped +
# noted, and COLL: still resolves to fs0 (so `dir mkrd.efi` via COLL: finds it).
sect MARK_COLLIDE MARK_NOOPT | grep -aqF "already mapped to another volume" \
    && ok "colliding label -> alias skipped with a note (no clobber)" \
    || no "colliding label not guarded (would repoint another volume's mapping)"
if sect MARK_COLLIDE MARK_NOOPT | grep -aqF "File Not Found"; then
    no "COLL: was repointed to the RAM disk (mkrd.efi vanished from it)"
else
    ok "COLL: still resolves to its original volume (not clobbered)"
fi

# --- 8. -a/--alias is gone: unknown flag, no disk created ---
sect MARK_NOOPT MARK_VERBOSE | grep -aqF "unknown flag -a" \
    && ok "-a is removed (reports 'unknown flag -a')" || no "-a not reported as unknown"
sect MARK_NOOPT MARK_VERBOSE | grep -aqF "label   : NOPT" \
    && no "NOPT disk was created despite the unknown -a flag" \
    || ok "no disk created when -a is passed"

# --- 9. -l columns: MAPPING (fsN) | LABEL/ALIAS (label:) | SIZE (NMB) | FS ---
sect MARK_LIST MARK_DESTROY | grep -aqE "MAPPING[[:space:]]+LABEL/ALIAS[[:space:]]+SIZE[[:space:]]+FSTYPE" \
    && ok "-l header is MAPPING/LABEL/ALIAS/SIZE/FSTYPE" || no "-l header not MAPPING/LABEL/ALIAS/SIZE/FSTYPE"
sect MARK_LIST MARK_DESTROY | grep -aqE "FS[0-9]+:[[:space:]]+ALPHA:[[:space:]]+[0-9]+MB[[:space:]]+FAT16" \
    && ok "-l row: MAPPING=FS<n>: LABEL=ALPHA: SIZE=NMB FS=FAT16" \
    || no "-l row not MAPPING(fsN)/LABEL(ALPHA:)/SIZE(NMB)/FS"

# --- 10. -d GAMMA destroys + unmaps; -l drops it; map GAMMA: gone ---
sect MARK_DESTROY MARK_LIST2 | grep -aqF "destroyed" \
    && ok "-d GAMMA reports destroyed" || no "-d GAMMA did not report destroyed"
sect MARK_DESTROY MARK_LIST2 | grep -aqF "unmapped" \
    && ok "-d GAMMA unmaps the dangling shell alias(es)" || no "-d GAMMA did not unmap"
if sect MARK_LIST2 MARK_QDEAD | grep -aqF "ALPHA" \
   && ! sect MARK_LIST2 MARK_QDEAD | grep -aqF "GAMMA"; then
    ok "-l after -d: GAMMA gone, ALPHA remains"
else
    no "-l after destroy did not drop GAMMA (or lost ALPHA)"
fi
sect MARK_QDEAD MARK_DONE | grep -aqF "Cannot find mapped device" \
    && ok "GAMMA: no longer mapped after destroy (alias removed)" \
    || no "GAMMA: still mapped after destroy (dangling alias)"

# --- 11. Quiet by default; -v enables the protocol probe ---
if sed -n '1,/MARK_VERBOSE/p' "$LOG" | grep -aqE '\[(INFO|DEBUG)\]'; then
    no "library log chatter leaked without -v"
else
    ok "no [INFO]/[DEBUG] chatter without -v"
fi
sect MARK_VERBOSE MARK_LIST | grep -aqF "PROBE" \
    && ok "-v enables diagnostics (protocol probe shown)" || no "-v did not enable diagnostics"

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): mkrd fsN-primary + label-alias mapping verified ==="; exit 0
else
    echo "=== FAIL ($ARCH) ==="; exit 1
fi
