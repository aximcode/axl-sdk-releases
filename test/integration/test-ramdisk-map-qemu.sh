#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0 guest=none
# guest=none: its subject is the Shell's volume map around the boot volume
#   fs0: (`mkrd fs0` must never clobber it, aliases mapped to fs0:, d.txt
#   absent from fs0 afterwards); on a leased guest fs0: is the pool's disk,
#   not the run's.
# test-ramdisk-map-qemu.sh — mkrd maps a freshly created RAM disk so it shows in
# a bare `map` IMMEDIATELY (no `map -r`), looking like a native volume:
#
#       FS20: Alias(s):DEMO:
#           VirtualDisk(...)
#
# It does this with TWO EFI_SHELL_PROTOCOL.SetMap calls on the disk's device
# path: a free `FS<n>` name, FS20 or above, FIRST (so the shell's name filter
# shows it and it becomes the PRIMARY column) and the volume LABEL second (so
# the label lands in the `Alias(s):` column). Both `FS<n>:` and `LABEL:` are
# usable paths with no `map -r`. There is NO `-a/--alias` option and NO
# `%<label>%` env var anymore — the label IS the friendly alias, and the fsN is
# the native handle.
#
# A ~20-check matrix over one QEMU boot:
#   - no-args / -h        -> print help; help no longer advertises -a
#   - `mkrd DEMO`         -> `map` shows `FS20: Alias(s):DEMO:`; DEMO: usable
#   - `mkrd DEMO` again   -> idempotent: same fsN + alias, "reused", no duplicate
#   - ALPHA/BETA/GAMMA    -> coexist, each `FS<n>: Alias(s):<LABEL>:`, distinct fsN
#   - `-l` columns MAPPING(fsN)/LABEL/ALIAS(label:)/SIZE/FS; `-d GAMMA` destroys+unmaps
#     and the next create reuses the fsN it freed
#   - label unusable as an alias (fs-pattern `fs9` / unsafe `BADX.Y` / over-long)
#     -> mapped as `FS<n>:` with NO label alias, and a note says why
#   - `mkrd fs0`          -> never clobbers the boot volume
#   - `mkrd X -a RD`      -> `-a` is gone: "unknown flag -a", no disk created
#   - `-v`                -> protocol probe; quiet (no [INFO]) without it
#   - a map name + %path% made BEFORE mkrd survive create+destroy, and create
#     warns that a later `map -r` replaces the names it set (mkrd runs none)
#   - a SECOND boot: `map -u` names an unmapped volume from the shell's own
#     counter (FS1) and leaves the FS20 mkrd took alone; a re-run says
#     "reused", gives back a missing fsN or label, reports the disk's real
#     size, and notes only names it set; with FS20..FS255 all taken it exits
#     0 and points at `map -u` (or, for a disk that keeps its label, at
#     `map -r`, since `map -u` skips a disk that has a name)
# This is the read-only-boot scratch-volume workflow (delldiags).
#
# Usage: ./test/integration/test-ramdisk-map-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^# Usage/p' "$0"; exit 0 ;;
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
MKRD="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")/tools/mkrd.efi"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" tools >/dev/null 2>&1 || true
[[ -f "$MKRD" ]] || { echo "ERROR: $MKRD not built"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"
LOG="$TMP/serial.log"
# NB: avoid "-r" / "()" in echo markers — the shell's echo treats a bare
# "-r" token as a flag. Use plain ASCII markers.
cat > "$NSH" <<'EOF'
@echo -off
fs0:
map PRESRV fs0:
echo MARK_PATH_BEFORE=[%path%]
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
echo MARK_REUSE
mkrd.efi DELTA -s 4
echo MARK_PRESERVE
map PRESRV:
echo MARK_PATH_AFTER=[%path%]
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
# The FS<n> in mkrd's create summary for label $2, first create in log $1.
fsn_of() {
    grep -aoE "mapping : FS[0-9]+: \(alias $2:\)" "$1" | head -1 \
        | grep -oE "FS[0-9]+" || true
}
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
# The first create in this boot takes the floor, FS20: not FS1, the number the
# shell's own `map -u` counter hands out next (check 13 covers why).
sect MARK_CREATE MARK_MAP_CREATE | grep -aqF "mapping : FS20: (alias DEMO:)" \
    && ok "the first create takes FS20, the floor, not the shell's next number" \
    || no "the first create did not take FS20"
if sect MARK_CREATE MARK_MAP_CREATE | grep -aqE "^  var |%DEMO%"; then
    no "create still emits a %VAR% (the '  var :' line / %DEMO%)"
else
    ok "create emits no %<label>% env var"
fi
# create runs no `map -r`, so the shell's mapping table cannot appear in its
# output — only the EXPLICIT `map` after MARK_MAP_CREATE prints one ("Mapping
# table" is the EDK2 `map` header). Contract, not a detector on this shell: the
# pre-fix refresh went through a nested, off-console Execute and was silent too.
if sect MARK_CREATE MARK_MAP_CREATE | grep -aqiF "Mapping table"; then
    no "create printed a mapping table — something ran `map -r`"
else
    ok "create prints no mapping table (it runs no map -r)"
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
# COLL: is fs0's name (mapped before `mkrd COLL`), not the COLL disk's, so -l
# must show that disk's label plain -- the colon means "type this to reach
# the disk", and COLL: reaches fs0.
sect MARK_LIST MARK_DESTROY | grep -aqE "FS[0-9]+:[[:space:]]+COLL[[:space:]]" \
    && ok "-l shows a label held by another volume without the colon" \
    || no "-l shows COLL: as the COLL disk's alias, but COLL: is fs0's name"

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
sect MARK_QDEAD MARK_REUSE | grep -aqF "Cannot find mapped device" \
    && ok "GAMMA: no longer mapped after destroy (alias removed)" \
    || no "GAMMA: still mapped after destroy (dangling alias)"
# The fsN a destroy frees is free in the shell's live map at once, so the next
# create takes it: mkrd keeps no counter between runs, it asks the map.
gamma_fsn=$(fsn_of "$LOG" GAMMA)
delta_fsn=$(fsn_of "$LOG" DELTA)
if [[ -n "$gamma_fsn" && "$delta_fsn" == "$gamma_fsn" ]]; then
    ok "the next create reuses $gamma_fsn, the name -d GAMMA freed"
else
    no "after -d GAMMA (${gamma_fsn:-no fsN}) the next create took ${delta_fsn:-no fsN}"
fi

# --- 11. Quiet by default; -v enables the protocol probe ---
# Scope the check to GUEST output — between the first guest marker (MARK_NOARGS)
# and MARK_VERBOSE. run-qemu writes host-side setup diagnostics (e.g.
# find_shell_efi's "[INFO] Extracted Shell.efi ..." when the Shell is extracted
# from firmware rather than found as a package) to its own stderr, which this
# test folds into $LOG via `2>&1`. Those host logs precede the guest boot, so a
# from-line-1 grep mistook them for guest library chatter — green locally (Shell
# is a system package, no extraction) but red in CI (Shell is extracted).
leaked=""
if sed -n '/MARK_NOARGS/,/MARK_VERBOSE/p' "$LOG" | grep -aqE '\[(INFO|DEBUG)\]'; then
    no "library log chatter leaked without -v"
    # Capture the offending lines; printed in the FAIL block below so they land
    # inside run-integration's last-25-lines failure view (a CI-only failure has
    # no other way to show the raw serial line and its source).
    leaked=$(sed -n '/MARK_NOARGS/,/MARK_VERBOSE/p' "$LOG" | grep -anE '\[(INFO|DEBUG)\]')
else
    ok "no [INFO]/[DEBUG] chatter without -v"
fi
sect MARK_VERBOSE MARK_LIST | grep -aqF "PROBE" \
    && ok "-v enables diagnostics (protocol probe shown)" || no "-v did not enable diagnostics"

# --- 12. mkrd NEVER rebuilds the shell's map ---
# mkrd used to run the shell's `map -r` from inside create/destroy
# (axl_map_refresh). That is not a local operation: ShellCommandCreateInitial-
# MappingsAndPaths() empties the WHOLE map list (every user-made name, and each
# mapping's remembered current directory) and blanks %path%, while `map -r`
# first calls ProbeForMediaChange() — a dummy ReadBlocks on every physical
# block device that lets each REPORT a media change, and one that does has its
# BlockIo re-installed. That is the mechanism matching a field report (an
# editor's "Read File Failed", truncated small files, a user map name needing a
# remap); the mechanism is EDK2-sourced, the causation was never isolated on
# hardware. PRESRV: is mapped by the SCRIPT before any mkrd runs; several
# creates and a destroy later it must still resolve, and %path% untouched.
# Both halves: no "cannot find" error AND the name actually listed among fs0's
# aliases, so a `map PRESRV:` that printed nothing at all cannot pass this.
if sect MARK_PRESERVE MARK_DONE | grep -aqiF "Cannot find mapped device"; then
    no "mkrd dropped a pre-existing map name -- it rebuilt the shell's map"
elif sect MARK_PRESERVE MARK_DONE | grep -aqE "Alias\(s\):.*PRESRV"; then
    ok "a map name made before mkrd survives create+destroy (no internal map -r)"
else
    no "map PRESRV: printed no mapping at all -- the probe did not run"
fi
# CONTRACT, not detectors: on a nesting shell the pre-fix build passed these
# two as well (Execute ran the map rebuild inside a nested image, and
# axl_map_refresh snapshotted and restored %path% itself). The detector for the
# real thing is in test-old-shell-qemu.sh, where Execute runs in-context. The
# [brackets] keep an empty %path% distinguishable from a missing marker.
path_before=$(grep -a 'MARK_PATH_BEFORE=' "$LOG" | head -1 | sed 's/.*MARK_PATH_BEFORE=//')
path_after=$(grep -a 'MARK_PATH_AFTER=' "$LOG" | head -1 | sed 's/.*MARK_PATH_AFTER=//')
if [[ -n "$path_before" && "$path_before" == "$path_after" ]]; then
    ok "%path% is unchanged across mkrd (it runs no map -r)"
else
    no "%path% changed across mkrd: before=$path_before after=$path_after"
fi
# The names are mkrd's own SetMap entries, so a later `map -r` reclaims them —
# create says so rather than leaving it to the docs.
sect MARK_CREATE MARK_MAP_CREATE | grep -aqF "a later 'map -r'" \
    && ok "create warns that a later 'map -r' replaces the names" \
    || no "create does not warn about a later 'map -r'"

# --- 13. `map -u` leaves mkrd's names alone (the FS20 floor) ---------------
# `map -u` names each volume that has no name FS<mFsMaxCount++>: a private
# counter that starts at the number of volumes named at boot and never looks at
# the map (ShellPkg ShellCommandCreateNewMappingName), and EfiShellSetMap
# deletes the previous holder of a name first. mkrd used to take the lowest
# free FS<n>, which is exactly that counter's next value, so the next volume
# `map -u` named took the disk's name. mkrd now starts at FS20.
#
# A SECOND boot, so the shell's counter is known: with one boot volume, the
# next name `map -u` hands out is FS1. UTWO's names are removed so it is an
# unmapped volume -- with every volume named, `map -u` skips them all and does
# nothing. Its fsN is removed under both numbers a build can give it (FS21 with
# the floor, FS2 without); the one that is not mapped just errors. That keeps
# the check about the steal rather than the number: against a build without
# the floor it fails at "keeps the name mkrd gave it", not at setup.
UNSH="$TMP/mapu.nsh"
ULOG="$TMP/mapu.log"
cat > "$UNSH" <<'EOF'
@echo -off
fs0:
mkrd.efi UONE -s 4
mkrd.efi UTWO -s 4
UONE:
echo uone-content > uone.txt
UTWO:
echo utwo-content > utwo.txt
fs0:
map -d UTWO:
map -d FS21:
map -d FS2:
echo MARK_U_BEFORE
map UONE:
mkrd.efi -l
echo MARK_U_MAPU
map -u
echo MARK_U_AFTER
map UONE:
echo MARK_U_NEW
dir FS1:\
echo MARK_U_LABEL
dir UONE:\
echo MARK_U_DONE
map -d FS20:
echo MARK_R_LABEL_ONLY
mkrd.efi UONE -s 4
map UONE:
echo MARK_R_UNMAPPED
map -d UONE:
map -d FS20:
mkrd.efi UONE -s 4
echo MARK_R_MAPR
map -r
echo MARK_R_AFTER_MAPR
mkrd.efi UONE -s 4
map UONE:
echo MARK_R_AGAIN
mkrd.efi UONE -s 8
echo MARK_R_SETUP_KEEP
mkrd.efi UKEEP -s 4
map -d FS20:
echo MARK_R_FILL
for %a run (20 255)
  map FS%a fs0: > nul
endfor
echo MARK_R_FULL
mkrd.efi UFULL -s 4
echo MARK_R_RC=%lasterror%
echo MARK_R_KEEP
mkrd.efi UKEEP -s 4
echo MARK_R_MAPU
map -u
mkrd.efi -l
echo MARK_R_DONE
reset -s
EOF
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$timeout" \
    --nsh "$UNSH" "$MKRD" > "$ULOG" 2>&1 || true

usect() { sed -n "/$1/,/$2/p" "$ULOG"; }
uname=$(fsn_of "$ULOG" UONE)
# `map UONE:` prints UONE's device as "<first name>: Alias(s):<the rest>".
holds_uname() { grep -aqE "(^|[[:space:]])$uname: Alias\(s\):.*UONE:"; }
# The "before" range stops ahead of `map -u`, which prints a mapping table.
if [[ -n "$uname" ]] && usect MARK_U_BEFORE MARK_U_MAPU | holds_uname; then
    ok "before map -u, UONE is mapped as $uname, the name mkrd gave it"
else
    no "setup failed: no 'mapping : FS<n>: (alias UONE:)', or map UONE: did not show it"
fi
if usect MARK_U_BEFORE MARK_U_MAPU | grep -aqE "\(unmapped\)[[:space:]]+UTWO[[:space:]]"; then
    ok "before map -u, UTWO has no shell name (mkrd -l: (unmapped))"
else
    no "setup failed: UTWO still had a shell name before map -u"
fi
# Control: map -u really named the unmapped volume, and from its own counter.
if usect MARK_U_NEW MARK_U_LABEL | grep -aqF "utwo.txt"; then
    ok "map -u named the unmapped volume FS1 (the shell's own counter)"
else
    no "map -u did not name the unmapped volume FS1 -- the check below proves nothing"
fi
if [[ -n "$uname" ]] && usect MARK_U_AFTER MARK_U_NEW | holds_uname; then
    ok "after map -u, UONE keeps $uname, the name mkrd gave it"
else
    no "map -u took UONE's ${uname:-FS<n>} -- mkrd picked a number the shell's counter hands out"
fi
if usect MARK_U_LABEL MARK_U_DONE | grep -aqF "uone.txt"; then
    ok "the disk is still reachable by its label after map -u"
else
    no "the label stopped resolving after map -u"
fi

# --- 14. A re-run gives back missing names and says what it did --------------
# Same boot. A re-run of an existing disk must say "reused", never "created",
# and give back whatever name it finds missing: the fsN when only the label is
# left (after `map -d FS20:`, or when `map -u` took the fsN), both when neither
# is, and the label after a `map -r` (which gives the disk an fsN of the shell's
# own and drops the label). The fsN stays the disk's FIRST name, the one a bare
# `map` shows as primary, so a restored label goes back behind it. The note
# about `map -r` is printed only when mkrd set a name in that run.
if usect MARK_R_LABEL_ONLY MARK_R_UNMAPPED | grep -aqF "RAM disk reused:" \
   && usect MARK_R_LABEL_ONLY MARK_R_UNMAPPED | grep -aqF "mapping : FS20: (alias UONE:)" \
   && usect MARK_R_LABEL_ONLY MARK_R_UNMAPPED | grep -aqE "(^|[[:space:]])FS20: Alias\(s\):UONE:"; then
    ok "label left, fsN gone: a re-run gives FS20 back as the primary name"
else
    no "label left, fsN gone: a re-run did not give FS20 back ahead of UONE:"
fi
if usect MARK_R_UNMAPPED MARK_R_MAPR | grep -aqF "RAM disk reused:" \
   && ! usect MARK_R_UNMAPPED MARK_R_MAPR | grep -aqF "RAM disk created:" \
   && usect MARK_R_UNMAPPED MARK_R_MAPR | grep -aqF "mapping : FS20: (alias UONE:)"; then
    ok "no names left: a re-run says reused and names the disk again"
else
    no "no names left: a re-run said created, or did not name the disk again"
fi
if usect MARK_R_AFTER_MAPR MARK_R_AGAIN | grep -aqF "RAM disk reused:" \
   && usect MARK_R_AFTER_MAPR MARK_R_AGAIN | grep -aqE "mapping : FS[0-9]+: \(alias UONE:\)" \
   && usect MARK_R_AFTER_MAPR MARK_R_AGAIN | grep -aqE "(^|[[:space:]])FS[0-9]+: Alias\(s\):.*UONE:"; then
    ok "after map -r: a re-run gives the label back"
else
    no "after map -r: a re-run did not give the label back"
fi
if usect MARK_R_AFTER_MAPR MARK_R_AGAIN | grep -aqF "a later 'map -r' drops UONE:" \
   && ! usect MARK_R_AFTER_MAPR MARK_R_AGAIN | grep -aqF "replaces these names"; then
    ok "after map -r: the note is about the label it set, not the shell's own fsN"
else
    no "after map -r: the note does not match what mkrd set (only the label)"
fi
if usect MARK_R_AGAIN MARK_R_SETUP_KEEP | grep -aqF "RAM disk reused:" \
   && ! usect MARK_R_AGAIN MARK_R_SETUP_KEEP | grep -aqF "Note:"; then
    ok "nothing missing: a re-run sets nothing and prints no note"
else
    no "nothing missing: a re-run still printed a note (or was not reported reused)"
fi
# The same re-run asks for -s 8: the disk that exists is 4 MB, and the
# summary must say so, and that -s was ignored, rather than echo the request.
if usect MARK_R_AGAIN MARK_R_SETUP_KEEP | grep -aqF "size    : 4 MB (FAT16)" \
   && usect MARK_R_AGAIN MARK_R_SETUP_KEEP | grep -aqF -- "-s 8 ignored"; then
    ok "re-run with another -s: reports the disk's real size, -s ignored"
else
    no "re-run with another -s: the summary did not report the disk's real size"
fi

# --- 15. No free fsN: the disk is still made, exit 0, and the advice works -----
# Every name from FS20 to FS255 is taken above (mapped to fs0; the output goes
# to nul, since 236 mapping tables would swamp the log), so mkrd cannot name
# UFULL. It must say so, exit 0 (the disk exists; a non-zero status would
# abort a calling script), and point at `map -u`, which then names it: UFULL
# has no name to lose, so `map -u` does not skip it.
if usect MARK_R_FULL MARK_R_RC | grep -aqF "no free fsN from FS20 to FS255" \
   && usect MARK_R_FULL MARK_R_RC | grep -aqF "run 'map -u'"; then
    ok "no free fsN: mkrd says so and points at map -u"
else
    no "no free fsN: mkrd did not name the cause and the follow-up"
fi
if grep -aqE "MARK_R_RC=(0x)?0([^0-9xa-fA-F]|$)" "$ULOG"; then
    ok "no free fsN: mkrd exits 0 (the disk was created)"
else
    no "no free fsN: mkrd exited non-zero: $(grep -ao 'MARK_R_RC=[^[:space:]]*' "$ULOG" | head -1)"
fi
if usect MARK_R_MAPU MARK_R_DONE | grep -aqE "(^|[[:space:]])FS[0-9]+:[[:space:]]+UFULL[[:space:]]"; then
    ok "no free fsN: the advised map -u names the disk"
else
    no "no free fsN: the disk still had no name after the advised map -u"
fi

# --- 16. No free fsN for a disk that keeps its label: no map -u advice ---------
# UKEEP was left with only its label (map -d FS20:) before FS20..FS255 were
# taken. A re-run cannot give it an fsN, and `map -u` skips a disk that has any
# name, so pointing at `map -u` would be advice that does nothing. mkrd must
# point at the name it has and at `map -r` (which drops that name) instead.
if usect MARK_R_KEEP MARK_R_MAPU | grep -aqF "reachable as UKEEP:" \
   && usect MARK_R_KEEP MARK_R_MAPU | grep -aqF "'map -u' skips a disk that already has a name" \
   && ! usect MARK_R_KEEP MARK_R_MAPU | grep -aqF "run 'map -u' to mount it"; then
    ok "label kept, no free fsN: points at the label and map -r, not map -u"
else
    no "label kept, no free fsN: advised map -u, which skips a disk with a name"
fi

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): mkrd fsN-primary + label-alias mapping verified ==="; exit 0
else
    if [[ -n "$leaked" ]]; then
        echo "leaked [INFO]/[DEBUG] chatter (line:text):"
        echo "$leaked" | sed 's/^/  /'
    fi
    echo "=== FAIL ($ARCH) ==="; exit 1
fi
