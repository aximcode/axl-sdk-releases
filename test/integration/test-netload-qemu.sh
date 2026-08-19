#!/bin/bash
# test-meta: arch=both needs= est=93 local-only=0
# test-netload-qemu.sh — netload tool: scaffold, NVRAM state machine, driver
# discovery, and the crash-recovery quarantine flow. Real driver-load/link/DHCP
# is HW-validated, not covered here (no NIC drivers in QEMU).
set -euo pipefail
ARCH="X64"
while [[ $# -gt 0 ]]; do case "$1" in --arch) ARCH="$2"; shift 2;; *) echo "bad arg $1"; exit 2;; esac; done
case "$ARCH" in X64) NAT=x64;; AARCH64) NAT=aa64;; *) echo "bad arch"; exit 2;; esac
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Sourced up here (not at first use further down) because the host-port
# allocator is needed by the mid-file netdev fixtures too.
source "$DIR/scripts/axl-common.sh"
TOOLS="$("$DIR/scripts/build-prefix.sh" --abs "$NAT")/tools"
make -C "$DIR" ARCH="$NAT" tools >/dev/null 2>&1 || true
[[ -x "$TOOLS/netload.efi" ]] || { echo "ERROR: netload.efi not built"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"; LOG="$TMP/serial.log"

# Stage 3 dummy "drivers" (any .efi; they are only listed, not loaded, in
# this test) under a driver dir on the image so `--list`'s default-resolved
# path (\drivers\<arch>) and an explicit `--dir` override both have something
# real to scan.
mkdir -p "$TMP/drv"
cp "$TOOLS/hexdump.efi" "$TMP/drv/Aaa.efi"
cp "$TOOLS/hexdump.efi" "$TMP/drv/Bbb.efi"
cp "$TOOLS/hexdump.efi" "$TMP/drv/Ccc.efi"

# Dependency co-load fixtures: a SECOND driver dir with a valid dependency
# sidecar (Nic and Nic2 each require dependency Comp; Solo is self-contained),
# plus a THIRD dir with a malformed sidecar to prove parse-error fallback.
# Kept separate from $TMP/drv so the no-sidecar tests above see today's
# standalone behavior unchanged. Real USB-RNDIS/CDC co-load is NOT exercised
# here (QEMU has no such device); these fixtures test only the parse +
# classify + on-demand-load + breadcrumb-order logic with dummy .efi.
mkdir -p "$TMP/cdrv" "$TMP/bdrv"
cp "$TOOLS/hexdump.efi" "$TMP/cdrv/Nic.efi"
cp "$TOOLS/hexdump.efi" "$TMP/cdrv/Nic2.efi"
cp "$TOOLS/hexdump.efi" "$TMP/cdrv/Comp.efi"
cp "$TOOLS/hexdump.efi" "$TMP/cdrv/SubComp.efi"
cp "$TOOLS/hexdump.efi" "$TMP/cdrv/Solo.efi"
# Two-level dependency tree: Nic/Nic2 -> Comp -> SubComp. A probe of Nic must
# transitively bring up SubComp (Comp's own dependency), and BOTH Comp and
# SubComp are dependency drivers ([dep], never menu picks) even though Comp is
# also a `name` entry (it has its own dependency).
printf '%s\n' "{ schema: 1, drivers: [ { name: 'Nic.efi', requires: [ 'Comp.efi' ] }, { name: 'Nic2.efi', requires: [ 'Comp.efi' ] }, { name: 'Comp.efi', requires: [ 'SubComp.efi' ] } ] }" \
  > "$TMP/cdrv/netload-drivers.json5"
cp "$TOOLS/hexdump.efi" "$TMP/bdrv/Solo.efi"
printf '%s\n' "{ this is not valid json5 ]" > "$TMP/bdrv/netload-drivers.json5"

# iPXE-last ordering fixture: a dedicated 4th dir with a driver name
# ("Aipxe.efi") that matches axl_net_driver_is_ipxe's filename heuristic
# ("ipxe" substring, case-insensitive) plus three plain names that sort
# AFTER it under plain byte-wise alphabetical comparison ('A' < 'B'/'R'/
# 'U'). A naive alphabetical sweep tries "Aipxe.efi" FIRST -- exactly the
# hazard that must never happen: iPXE's LoadImage hook breaks every
# subsequent .efi load in the same session, so an iPXE candidate must
# always be tried LAST regardless of where its name falls alphabetically.
mkdir -p "$TMP/idrv"
cp "$TOOLS/hexdump.efi" "$TMP/idrv/Aipxe.efi"
cp "$TOOLS/hexdump.efi" "$TMP/idrv/Bcm.efi"
cp "$TOOLS/hexdump.efi" "$TMP/idrv/Rtk.efi"
cp "$TOOLS/hexdump.efi" "$TMP/idrv/Usb.efi"

{
  echo '@echo -off'; echo 'fs0:'
  echo 'echo MARK_HELP';   echo 'netload.efi --help'
  echo 'echo MARK_CLR';    echo 'netload.efi --clear'
  echo 'echo MARK_MARK';   echo 'netload.efi --_mark BadDrv.efi'
  echo 'echo MARK_RECOVER';echo 'netload.efi --dump'
  # Second quarantine cycle, targeting one of the staged drivers, so --list
  # has a real [crashed] tag to assert on.
  echo 'echo MARK_CLR2';   echo 'netload.efi --clear'
  echo 'echo MARK_MARK2';  echo 'netload.efi --_mark Bbb.efi'
  echo 'echo MARK_DUMP2';  echo 'netload.efi --dump'
  echo 'echo MARK_LIST';   echo "netload.efi --list --dir fs0:\\drivers\\$NAT"
  # Per-driver probe seam: clear NVRAM, then probe one dummy driver by name.
  # A dummy .efi (hexdump.efi) is a valid PE axl_driver_load can load, but
  # axl_driver_start won't bind a NIC -- outcome is PR_LOAD_FAIL/PR_NO_NIC,
  # and critically the breadcrumb is set before load and cleared after (no
  # crash), so the dummy must NOT end up quarantined.
  echo 'echo MARK_PROBE';  echo 'netload.efi --clear'
  echo "netload.efi --probe Aaa.efi --dir fs0:\\drivers\\$NAT"
  echo 'echo MARK_AFTER';  echo 'netload.efi --dump'
  # Auto sweep: quarantine Bbb.efi via a fresh crash cycle, then sweep -a
  # across all three staged dummies. Dummies never bring up a NIC, so the
  # sweep must skip Bbb.efi, probe Aaa.efi/Ccc.efi, and end with the
  # no-lease summary (real lease-acquired stop is HW-validated).
  echo 'echo MARK_AUTO_SETUP'; echo 'netload.efi --clear'
  echo 'echo MARK_AUTO_MARK';  echo 'netload.efi --_mark Bbb.efi'
  echo 'echo MARK_AUTO_DUMP';  echo 'netload.efi --dump'
  echo 'echo MARK_AUTO';       echo "netload.efi -a --dir fs0:\\drivers\\$NAT"
  # iPXE-last ordering (see the idrv fixture comment above): the -a sweep
  # across drivers4 must try Aipxe.efi LAST even though it sorts FIRST
  # alphabetically among the four staged names.
  echo 'echo MARK_IPXE_ORDER_SETUP'; echo 'netload.efi --clear'
  echo 'echo MARK_IPXE_ORDER'; echo "netload.efi -a --dir fs0:\\drivers4\\$NAT"
  echo 'echo MARK_IPXE_ORDER_DONE'
  echo 'echo MARK_KEY'    # section boundary for the -a sweep assertions above
  # Firmware-first: --connect tries the firmware's own NIC drivers (connect -r +
  # DHCP) with no staging. QEMU here has no NIC (no --net), so it reports none
  # bound -- the plumbing (connect + enumerate + report) is what we assert; a
  # real firmware NIC lease is HW-only.
  echo 'echo MARK_FW_CONNECT'; echo 'netload.efi --connect'
  echo 'echo MARK_FW_KEY'   # section boundary for the --connect assertion
  # Dependency co-load (against the drivers2 sidecar dir). Clear NVRAM first so
  # the quarantine sub-test starts clean. Real NIC bring-up is HW-only; these
  # exercise parse/classify/order/reuse/--no-deps/quarantine-warn logic.
  echo 'echo MARK_COMP_CLR';    echo 'netload.efi --clear'
  echo 'echo MARK_COMP_LIST';   echo "netload.efi --list --dir fs0:\\drivers2\\$NAT"
  echo 'echo MARK_COMP_PROBE';  echo "netload.efi --probe Nic.efi --dir fs0:\\drivers2\\$NAT"
  echo 'echo MARK_COMP_NOSUP';  echo "netload.efi --probe Nic.efi --no-deps --dir fs0:\\drivers2\\$NAT"
  echo 'echo MARK_COMP_AUTO';   echo "netload.efi -a --dir fs0:\\drivers2\\$NAT"
  # Quarantine the dependency (crash breadcrumb + recover), then probe a NIC
  # that needs it: netload must warn precisely and skip the dependency.
  echo 'echo MARK_COMP_QMARK';  echo 'netload.efi --_mark Comp.efi'
  echo 'echo MARK_COMP_QREC';   echo 'netload.efi --dump'
  echo 'echo MARK_COMP_QPROBE'; echo "netload.efi --probe Nic.efi --dir fs0:\\drivers2\\$NAT"
  # Malformed sidecar -> fall back to standalone listing.
  echo 'echo MARK_COMP_BAD';    echo "netload.efi --list --dir fs0:\\drivers3\\$NAT"
  # End-of-sweep findings report: clear, quarantine a CANDIDATE (Nic2), then
  # sweep. The summary must table every driver + outcome, count them, flag the
  # skipped/quarantined one with a --clear hint, show the co-loaded dep
  # driver, end on NO DHCP LEASE, and persist a SWEEP line to the NVRAM log.
  echo 'echo MARK_SUM_CLR';   echo 'netload.efi --clear'
  echo 'echo MARK_SUM_QMARK'; echo 'netload.efi --_mark Nic2.efi'
  echo 'echo MARK_SUM_QREC';  echo 'netload.efi --dump'
  echo 'echo MARK_SUM_AUTO';  echo "netload.efi -a --dir fs0:\\drivers2\\$NAT"
  echo 'echo MARK_SUM_DUMP';  echo 'netload.efi --dump'
  # --- Config validation guards (netload_cfg_parse) -- pure argument
  # validation, no driver dir / NIC needed; errors before any bring-up.
  echo 'echo MARK_ERR_IP_NOSEL'; echo "netload.efi --ip 10.0.0.5 -a --dir fs0:\\drivers\\$NAT"
  echo 'echo MARK_ERR_MACNIC';   echo "netload.efi --mac 00:11:22:33:44:55 --nic 0 --ip 10.0.0.5"
  echo 'echo MARK_ERR_BADIP';    echo "netload.efi --nic 0 --ip 999.0.0.1"
  echo 'echo MARK_ERR_MASK';     echo "netload.efi --nic 0 --ip 10.0.0.5 --mask 999.0.0.0"
  echo 'echo MARK_ERR_GW';       echo "netload.efi --nic 0 --ip 10.0.0.5 --gw nope"
  echo 'echo MARK_ERR_DNS';      echo "netload.efi --nic 0 --dns 300.1.1.1"
  echo 'echo MARK_ERR_DT';       echo "netload.efi --nic 0 --dhcp-timeout abc"
  echo 'echo MARK_ERR_RT';       echo "netload.efi --nic 0 --retries 0"
  # Overflow regression: a value that overflows uint32_t (2^32+1) must be
  # REJECTED, not silently wrapped (4294967297 mod 2^32 == 1). AXL_ARG_U32's
  # type-range check catches this before netload ever sees the value.
  echo 'echo MARK_ERR_OVERFLOW'; echo "netload.efi --nic 4294967297 --ip 10.0.0.5"
  echo 'echo MARK_HELP_SHORT';   echo 'netload.efi -h'
  # --- PR_NO_REACH rendering (structural; real --ping/--ping-gw/--resolve
  # verification against a live NIC is HW-only). --_log is a headless seam
  # that seeds one NVRAM result-log line without any driver/NIC involved, so
  # --dump's log-token-to-label rendering can be asserted in QEMU.
  echo 'echo MARK_NOREACH_SEED'; echo 'netload.efi --clear'
  echo 'netload.efi --_log NOREACH Rtk.efi'
  echo 'echo MARK_NOREACH_DUMP'; echo 'netload.efi --dump'
  # --- Config save/apply (structural; a real load+apply needs a real driver
  # binding, which is HW-only -- see test-meta header). --_saveconf is a
  # headless seam that appends one line to the Config NVRAM var (no real win
  # needed), so the serialize/persist/--dump/--clear round-trip is asserted
  # here. The Config wire format is `key=value` text (AxlConfigFile), one
  # key per line; the UEFI shell has no way to embed a literal newline in a
  # single command-line argument, so the seam appends one key=value pair per
  # --_saveconf call rather than seeding the whole blob in one shot.
  # --_applydry below confirms the round-trip is clean.
  echo 'echo MARK_SAVE';      echo 'netload.efi --clear'
  echo 'netload.efi --_saveconf driver=Rtk.efi'
  echo 'netload.efi --_saveconf method=static'
  echo 'netload.efi --_saveconf ip=10.0.0.5'
  echo 'netload.efi --_saveconf mask=255.255.255.0'
  echo 'netload.efi --_saveconf gw=10.0.0.1'
  echo 'netload.efi --_saveconf mac=60:7d:09:57:f8:bf'
  echo 'echo MARK_SAVE_DUMP'; echo 'netload.efi --dump'
  # --_applydry runs config_load() ALONE (the AxlConfigFile parse + field
  # validation that --dump's raw-byte print never exercises) and echoes the
  # parsed fields, so a valid Config round-trips through the parser and a
  # garbage one is rejected.
  echo 'echo MARK_APPLYDRY';      echo 'netload.efi --_applydry'
  echo 'echo MARK_SAVE_CLR';  echo 'netload.efi --clear'
  echo 'echo MARK_SAVE_GONE'; echo 'netload.efi --dump'
  # Malformed Config: config_load must reject it, not crash or parse garbage.
  # A line with no '=' parses to an empty map (same as an absent Config var),
  # exactly like an old pre-AxlConfigFile positional Config would on upgrade.
  echo 'echo MARK_BADCFG_SEED'; echo 'netload.efi --_saveconf notavalidconfig'
  echo 'echo MARK_BADCFG_DRY';  echo 'netload.efi --_applydry'
  echo 'echo MARK_BADCFG_CLR';  echo 'netload.efi --clear'
  # --_fwrow: the firmware summary-row detail wording. QEMU's OVMF reports every
  # NIC link-up, so "firmware NICs present but all link-DOWN" (the real Dell
  # R6725 case: 4 link-down Broadcoms) is otherwise unreachable in QEMU. The
  # seam drives print_row_detail directly. N>0 must say the NICs are present and
  # down (NOT the misleading "no firmware NIC bound"); N=0 keeps "no firmware NIC
  # bound" for the genuinely-absent case.
  echo 'echo MARK_FWROW_DOWN'; echo 'netload.efi --_fwrow 4'; echo 'MARK_FWROW_DOWN_DONE'
  echo 'echo MARK_FWROW_NONE'; echo 'netload.efi --_fwrow 0'; echo 'MARK_FWROW_NONE_DONE'
  # --json: --probe on the dummy Aaa.efi (no NIC ever comes up) still emits a
  # single machine-readable result object AND keeps the human progress logs
  # (--json only suppresses the decorative summary tables, which --probe
  # doesn't print anyway -- this pins that progress survives regardless).
  echo 'echo MARK_JSON'; echo "netload.efi --probe Aaa.efi --json --dir fs0:\\drivers\\$NAT"
  # --json escaping regression: a driver name containing a raw backslash must
  # come out JSON-escaped ("\\") in the "driver" field. --probe passes the
  # name straight through (no filesystem lookup needed to reach probe_driver's
  # rep->name assignment -- the load itself fails, but the report row is still
  # built and --json'd), so this needs no fixture beyond an existing --dir. A
  # hand-rolled predecessor of print_json_result interpolated CLI strings
  # unescaped, so this exact line used to come out as invalid JSON.
  echo 'echo MARK_JSON_ESC'; echo "netload.efi --probe A\\B.efi --json --dir fs0:\\drivers\\$NAT"
  # --json regression: a no-win -a sweep whose LAST candidate is quarantined
  # must NOT emit a false "result":"up". Quarantine Ccc.efi (the last of the
  # sorted Aaa/Bbb/Ccc trio), then sweep -a --json: no dummy brings up a NIC,
  # so there is no win, and reports[nr-1] is the skipped Ccc row (result==0==
  # PR_OK unless the representative-selection + skipped guard hold).
  echo 'echo MARK_JSONQ_CLR';   echo 'netload.efi --clear'
  echo 'echo MARK_JSONQ_MARK';  echo 'netload.efi --_mark Ccc.efi'
  echo 'echo MARK_JSONQ_REC';   echo 'netload.efi --dump'
  echo 'echo MARK_JSONQ';       echo "netload.efi -a --json --dir fs0:\\drivers\\$NAT"
  # --- Output tee (-o/--out), full diagnostic dump (--diag), drivers-in-dump ---
  # -o tees netload's OWN output to a file on the writable boot volume (disk.img);
  # read it back with `type` to prove the tee wrote it.
  echo 'echo MARK_OUT_TEE';  echo "netload.efi --list -o nltee.txt --dir fs0:\\drivers\\$NAT"
  echo 'echo MARK_OUT_TYPE'; echo 'type nltee.txt'
  # --diag: netload's own network-landscape section + the shell 'drivers'
  # invoked via EFI_SHELL_PROTOCOL.Execute (present under the OVMF shell running
  # this startup.nsh). The "shell unavailable" fallback must NOT appear, and the
  # default --diag must NOT run 'dh -v' (that is the --dh opt-in only -- driving
  # 'dh -v' through the shell redirect RSOD'd a real Dell PowerEdge shell).
  echo 'echo MARK_DIAG';     echo 'netload.efi --diag'
  # Redirect path (the gap that let the RSOD slip in): --diag -o FILE must tee
  # netload's landscape AND append the shell 'drivers' output to FILE.
  echo 'echo MARK_DGOUT';      echo 'netload.efi --diag -o dgt.txt'
  echo 'echo MARK_DGTYPE'; echo 'type dgt.txt'
  # --dh opt-in: full 'dh -v' to SCREEN only (never through the crashing redirect).
  echo 'echo MARK_DGDH';  echo 'netload.efi --diag --dh'
  # -a -d composes: --diag is a REPORT that runs AFTER the -a sweep (not instead
  # of it). Both the auto sweep AND the diagnostic dump must appear.
  echo 'echo MARK_DGAUTO'; echo "netload.efi -a -d --dir fs0:\\drivers\\$NAT"
  # --dump now appends the shell 'drivers' list after the NVRAM findings.
  echo 'echo MARK_DUMP_DRV'; echo 'netload.efi --dump'
  echo 'echo MARK_DONE';   echo 'reset -s'
} > "$NSH"
"$DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 60 --nsh "$NSH" \
  --extra "$TMP/drv/Aaa.efi:drivers/$NAT/Aaa.efi" \
  --extra "$TMP/drv/Bbb.efi:drivers/$NAT/Bbb.efi" \
  --extra "$TMP/drv/Ccc.efi:drivers/$NAT/Ccc.efi" \
  --extra "$TMP/cdrv/Nic.efi:drivers2/$NAT/Nic.efi" \
  --extra "$TMP/cdrv/Nic2.efi:drivers2/$NAT/Nic2.efi" \
  --extra "$TMP/cdrv/Comp.efi:drivers2/$NAT/Comp.efi" \
  --extra "$TMP/cdrv/SubComp.efi:drivers2/$NAT/SubComp.efi" \
  --extra "$TMP/cdrv/Solo.efi:drivers2/$NAT/Solo.efi" \
  --extra "$TMP/cdrv/netload-drivers.json5:drivers2/$NAT/netload-drivers.json5" \
  --extra "$TMP/bdrv/Solo.efi:drivers3/$NAT/Solo.efi" \
  --extra "$TMP/bdrv/netload-drivers.json5:drivers3/$NAT/netload-drivers.json5" \
  --extra "$TMP/idrv/Aipxe.efi:drivers4/$NAT/Aipxe.efi" \
  --extra "$TMP/idrv/Bcm.efi:drivers4/$NAT/Bcm.efi" \
  --extra "$TMP/idrv/Rtk.efi:drivers4/$NAT/Rtk.efi" \
  --extra "$TMP/idrv/Usb.efi:drivers4/$NAT/Usb.efi" \
  "$TOOLS/netload.efi" > "$LOG" 2>&1 || true
# Assertions use `sect … | grep -q`; grep -q closes the pipe on first match, so
# sed takes SIGPIPE (exit 141) and pipefail would propagate that as a spurious
# pipeline failure (timing-dependent — bites the larger aa64 log). Every check
# below is guarded by `&& … || …`, so drop pipefail here; set -e still holds.
set +o pipefail
fail=0
sect() { sed -n "/$1/,/$2/p" "$LOG"; }
sect MARK_HELP MARK_DONE | grep -aqiE "netload" && echo "PASS: help runs" || { echo "FAIL: help"; fail=1; }
# --_mark seeds the breadcrumb; the NEXT run's --dump must detect the dangling
# breadcrumb, quarantine BadDrv.efi, and report it (crash recovery).
sect MARK_RECOVER MARK_DUMP2 | grep -aqiE "CRASH.*BadDrv\.efi|quarantin.*BadDrv\.efi" \
  && echo "PASS: crash breadcrumb -> quarantine" || { echo "FAIL: crash recovery"; fail=1; }
sect MARK_RECOVER MARK_DUMP2 | grep -aqiE "BadDrv\.efi" \
  && echo "PASS: quarantine listed in --dump" || { echo "FAIL: dump quarantine"; fail=1; }
sect MARK_LIST MARK_PROBE | grep -aqE "Aaa\.efi" && sect MARK_LIST MARK_PROBE | grep -aqE "Ccc\.efi" \
  && echo "PASS: driver scan lists staged .efi" || { echo "FAIL: driver scan"; fail=1; }
# Bbb.efi was quarantined by the MARK_MARK2/MARK_DUMP2 cycle above; --list
# must tag it [crashed] while Aaa.efi/Ccc.efi (never quarantined) are not.
sect MARK_LIST MARK_PROBE | grep -aqE "Bbb\.efi.*\[crashed\]" \
  && echo "PASS: quarantined driver tagged [crashed]" || { echo "FAIL: is_quarantined tag"; fail=1; }
sect MARK_LIST MARK_PROBE | grep -aqE "Aaa\.efi.*\[crashed\]" \
  && { echo "FAIL: Aaa.efi wrongly tagged [crashed]"; fail=1; } || echo "PASS: Aaa.efi not tagged [crashed]"
sect MARK_LIST MARK_PROBE | grep -aqE "Ccc\.efi.*\[crashed\]" \
  && { echo "FAIL: Ccc.efi wrongly tagged [crashed]"; fail=1; } || echo "PASS: Ccc.efi not tagged [crashed]"
# --probe: the dummy driver survives a load it cannot bind as a NIC. The
# breadcrumb must be set/logged before load, and cleared after (no crash).
sect MARK_PROBE MARK_DONE | grep -aqiE "loading Aaa\.efi" \
  && echo "PASS: probe logs the driver before load" || { echo "FAIL: probe log"; fail=1; }
# Post-probe --dump: quarantine must still be empty -- a survived (non-
# crashing) load must never quarantine the driver.
sect MARK_AFTER MARK_DONE | grep -aqiE "\(empty\)" \
  && echo "PASS: breadcrumb cleared after survived load (quarantine empty)" \
  || { echo "FAIL: breadcrumb clear / quarantine not empty"; fail=1; }
# Auto sweep (-a): Bbb.efi was freshly quarantined by the MARK_AUTO_MARK/
# MARK_AUTO_DUMP crash-recovery cycle above; the sweep must skip it and
# probe the non-quarantined dummies instead. Windows end at MARK_KEY (the
# first marker after the auto block) so each assertion is scoped to its
# own output, not the whole rest of the log.
sect MARK_AUTO MARK_KEY | grep -aqiE "skip.*Bbb\.efi|Bbb\.efi.*\[crashed\]" \
  && echo "PASS: auto skips quarantined" || { echo "FAIL: auto skip"; fail=1; }
sect MARK_AUTO MARK_KEY | grep -aqiE "loading Aaa\.efi" \
  && echo "PASS: auto probes non-quarantined" || { echo "FAIL: auto probe"; fail=1; }
# ...and continues PAST the mid-sweep skip to Ccc.efi (alphabetically after
# the quarantined Bbb.efi) -- proves the skip didn't early-exit the sweep.
sect MARK_AUTO MARK_KEY | grep -aqiE "loading Ccc\.efi" \
  && echo "PASS: auto continues past skip to Ccc.efi" || { echo "FAIL: auto continue past skip"; fail=1; }
# Dummies never bring up a NIC -- the sweep must end with the no-lease summary.
sect MARK_AUTO MARK_KEY | grep -aqiE "no driver acquired a DHCP lease" \
  && echo "PASS: auto sweep ends without a lease" || { echo "FAIL: auto no-lease summary"; fail=1; }
# --- iPXE-last ordering (root-cause fix: axl_net_driver_is_ipxe + cmp_name) ---
# Aipxe.efi sorts FIRST among {Aipxe,Bcm,Rtk,Usb} under plain byte-wise
# alphabetical comparison ('A' < 'B'/'R'/'U'), so a naive alphabetical sweep
# tries it first -- exactly the hazard that corrupts every later .efi load in
# the session via iPXE's LoadImage hook. The fix must force it LAST
# regardless of alphabetical position: assert its "loading" line comes AFTER
# every other candidate's, using the same line-number-order technique as the
# dependency/NIC ordering check above.
al=$(sect MARK_IPXE_ORDER MARK_IPXE_ORDER_DONE | grep -aniE "loading Aipxe\.efi" | head -1 | cut -d: -f1 || true)
bl=$(sect MARK_IPXE_ORDER MARK_IPXE_ORDER_DONE | grep -aniE "loading Bcm\.efi" | head -1 | cut -d: -f1 || true)
rl=$(sect MARK_IPXE_ORDER MARK_IPXE_ORDER_DONE | grep -aniE "loading Rtk\.efi" | head -1 | cut -d: -f1 || true)
ul=$(sect MARK_IPXE_ORDER MARK_IPXE_ORDER_DONE | grep -aniE "loading Usb\.efi" | head -1 | cut -d: -f1 || true)
[[ -n "$al" && -n "$bl" && -n "$rl" && -n "$ul" && "$al" -gt "$bl" && "$al" -gt "$rl" && "$al" -gt "$ul" ]] \
  && echo "PASS: iPXE-named candidate tried LAST despite sorting first alphabetically" \
  || { echo "FAIL: iPXE ordering broken (Aipxe=$al Bcm=$bl Rtk=$rl Usb=$ul) -- expected Aipxe last"; fail=1; }
# --- Firmware-first probe (try the firmware's own NIC drivers before staging) ---
# -a runs the firmware-first probe before the staged sweep.
sect MARK_AUTO MARK_KEY | grep -aqiE "trying the firmware" \
  && echo "PASS: -a tries firmware drivers first" || { echo "FAIL: -a firmware-first"; fail=1; }
# reports[0] on the NO-NET boot: the -a summary must carry the firmware-first
# row even with no NIC present -- tagged "firmware:no NIC" with the fitting
# detail "no firmware NIC bound" (exact strings; this regression-guards the
# PR_NO_NIC + is_firmware rendering branch, the counterpart to the --net
# firmware:LEASED assertion below).
sect MARK_AUTO MARK_KEY | grep -aqE "firmware:no NIC" \
  && echo "PASS: no-net -a summary shows the firmware:no NIC row (reports[0])" \
  || { echo "FAIL: no-net firmware:no NIC label"; fail=1; }
sect MARK_AUTO MARK_KEY | grep -aqE "no firmware NIC bound" \
  && echo "PASS: no-net firmware row detail reads 'no firmware NIC bound'" \
  || { echo "FAIL: no-net firmware NIC bound detail"; fail=1; }
# --connect: firmware-only, no staging. No NIC in QEMU -> reports none bound.
sect MARK_FW_CONNECT MARK_FW_KEY | grep -aqiE "trying the firmware" \
  && echo "PASS: --connect runs the firmware probe" || { echo "FAIL: --connect probe"; fail=1; }
sect MARK_FW_CONNECT MARK_FW_KEY | grep -aqiE "no network interfaces|no firmware NIC driver bound" \
  && echo "PASS: --connect reports no firmware NIC (QEMU has none)" || { echo "FAIL: --connect no-NIC report"; fail=1; }
# --- Dependency co-load (structural; real USB-RNDIS/CDC bring-up is HW-only) ---
# Classification: Comp.efi is a dependency driver (tagged [dep], filtered from
# picks); Nic/Nic2/Solo are NIC candidates and must NOT be tagged dep.
sect MARK_COMP_LIST MARK_COMP_PROBE | grep -aqiE "Comp\.efi.*\[dep\]" \
  && echo "PASS: dependency driver tagged [dep]" || { echo "FAIL: dep tag"; fail=1; }
sect MARK_COMP_LIST MARK_COMP_PROBE | grep -aqE "Nic\.efi" \
  && echo "PASS: NIC candidate listed" || { echo "FAIL: candidate listed"; fail=1; }
sect MARK_COMP_LIST MARK_COMP_PROBE | grep -aqiE "Nic\.efi.*\[dep\]" \
  && { echo "FAIL: Nic.efi wrongly tagged [dep]"; fail=1; } || echo "PASS: Nic.efi not tagged dep"
# A mid-tree node (Comp: a dependency that itself has a dependency) and a leaf
# (SubComp) are BOTH [dep] -- required-by-someone means auto-loaded, not a pick.
sect MARK_COMP_LIST MARK_COMP_PROBE | grep -aqiE "SubComp\.efi.*\[dep\]" \
  && echo "PASS: transitive leaf tagged [dep]" || { echo "FAIL: SubComp not tagged dep"; fail=1; }
# On-demand co-load + ordering: the dependency loads BEFORE the NIC (so exactly
# one driver is ever in the breadcrumb per load -> single-culprit preserved).
sect MARK_COMP_PROBE MARK_COMP_NOSUP | grep -aqiE "loading dependency Comp\.efi" \
  && echo "PASS: dependency auto-loaded on probe" || { echo "FAIL: dependency load"; fail=1; }
# Transitive: probing Nic (-> Comp -> SubComp) must bring up SubComp too.
sect MARK_COMP_PROBE MARK_COMP_NOSUP | grep -aqiE "loading dependency SubComp\.efi" \
  && echo "PASS: transitive dependency (dep-of-dep) auto-loaded" || { echo "FAIL: transitive dep load"; fail=1; }
# Connect-stall warning: probing a dependency-dependent NIC warns the operator
# the RNDIS/USB-NIC connect can take up to ~60s and NOT to reset (so a bounded
# firmware stall isn't mistaken for a hang and power-cycled).
sect MARK_COMP_PROBE MARK_COMP_NOSUP | grep -aqiE "do not reset" \
  && echo "PASS: connect-stall wait warning shown" || { echo "FAIL: wait warning"; fail=1; }
cl=$(sect MARK_COMP_PROBE MARK_COMP_NOSUP | grep -aniE "loading dependency Comp\.efi" | head -1 | cut -d: -f1 || true)
nl=$(sect MARK_COMP_PROBE MARK_COMP_NOSUP | grep -aniE "loading Nic\.efi" | head -1 | cut -d: -f1 || true)
[[ -n "$cl" && -n "$nl" && "$cl" -lt "$nl" ]] \
  && echo "PASS: dependency loaded before the NIC" || { echo "FAIL: dependency/NIC order (cl=$cl nl=$nl)"; fail=1; }
# --no-deps suppresses the dependency load but still probes the NIC.
sect MARK_COMP_NOSUP MARK_COMP_AUTO | grep -aqiE "loading dependency" \
  && { echo "FAIL: --no-deps still loaded a dependency"; fail=1; } || echo "PASS: --no-deps suppresses dependency"
sect MARK_COMP_NOSUP MARK_COMP_AUTO | grep -aqiE "loading Nic\.efi" \
  && echo "PASS: --no-deps still probes the NIC" || { echo "FAIL: --no-deps probe"; fail=1; }
# --no-deps: no dependency resident -> no RNDIS-init connect stall -> no warning.
sect MARK_COMP_NOSUP MARK_COMP_AUTO | grep -aqiE "do not reset" \
  && { echo "FAIL: --no-deps wrongly warned of connect stall"; fail=1; } || echo "PASS: --no-deps suppresses wait warning"
# Under --no-deps a NO_NIC result DOES point at the un-loaded dependency (that's
# the actionable cause when auto-load is off).
sect MARK_COMP_NOSUP MARK_COMP_AUTO | grep -aqiE "no-deps.*dependency|dependency.*not auto-loaded" \
  && echo "PASS: --no-deps NO_NIC cites the un-loaded dependency" || { echo "FAIL: --no-deps NO_NIC hint"; fail=1; }
# Reuse: across the -a sweep of Nic + Nic2 (both need Comp), the dependency
# loads exactly ONCE (resident, reused).
cc=$(sect MARK_COMP_AUTO MARK_COMP_QMARK | grep -aciE "loading dependency Comp\.efi" || true)
[[ "$cc" -eq 1 ]] \
  && echo "PASS: dependency loaded once, reused across sweep" || { echo "FAIL: dependency reuse (got $cc)"; fail=1; }
sect MARK_COMP_AUTO MARK_COMP_QMARK | grep -aqiE "loading Nic2\.efi" \
  && echo "PASS: sweep probes second dependency NIC" || { echo "FAIL: sweep Nic2"; fail=1; }
# In DEFAULT (auto-load) mode a NO_NIC must NOT claim a dependency is needed --
# any declared dependency was already loaded, so the cause is no matching HW.
# (The "loading dependency" co-load line is fine; the ban is on "needs a dep".)
sect MARK_COMP_AUTO MARK_COMP_QMARK | grep -aqiE "needs a dependency|missing dependency|dependency.*not auto-loaded" \
  && { echo "FAIL: auto-mode NO_NIC wrongly blamed a dependency"; fail=1; } || echo "PASS: auto-mode NO_NIC does not blame a dependency"
# Quarantined dependency: precise warning naming the NIC, dependency NOT loaded,
# but the NIC is still probed (it may be self-contained on this box).
sect MARK_COMP_QPROBE MARK_COMP_BAD | grep -aqiE "Comp\.efi.*quarantin.*Nic\.efi|dependency Comp\.efi is quarantined" \
  && echo "PASS: quarantined dependency warned precisely" || { echo "FAIL: quarantine dependency warn"; fail=1; }
sect MARK_COMP_QPROBE MARK_COMP_BAD | grep -aqiE "loading dependency Comp\.efi" \
  && { echo "FAIL: loaded a quarantined dependency"; fail=1; } || echo "PASS: quarantined dependency not loaded"
sect MARK_COMP_QPROBE MARK_COMP_BAD | grep -aqiE "loading Nic\.efi" \
  && echo "PASS: NIC still probed despite quarantined dependency" || { echo "FAIL: probe past quarantined dependency"; fail=1; }
# Malformed sidecar: fall back to standalone listing (Solo still shown).
sect MARK_COMP_BAD MARK_SUM_CLR | grep -aqE "Solo\.efi" \
  && echo "PASS: malformed sidecar falls back to standalone" || { echo "FAIL: parse-error fallback"; fail=1; }
# --- End-of-sweep findings report (structural; real link/lease detail is HW-only) ---
# The -a summary must render a table header, list every probed candidate + the
# skipped one, name the co-loaded dependency driver, count them, and end NO LEASE.
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "netload summary" \
  && echo "PASS: summary report header" || { echo "FAIL: summary header"; fail=1; }
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqE "Nic\.efi" && sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqE "Solo\.efi" \
  && echo "PASS: summary lists probed candidates" || { echo "FAIL: summary candidate rows"; fail=1; }
# Nic2 was quarantined -> it must show as skipped in the summary, not probed.
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "Nic2\.efi.*(skip|quarantin)" \
  && echo "PASS: summary marks quarantined candidate skipped" || { echo "FAIL: summary skip row"; fail=1; }
# The co-loaded dependency appears as a dependency row.
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "Comp\.efi.*dependency|dependency.*Comp\.efi" \
  && echo "PASS: summary shows co-loaded dependency driver" || { echo "FAIL: summary dependency row"; fail=1; }
# Count line + outcome.
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "leased" \
  && echo "PASS: summary count line" || { echo "FAIL: summary counts"; fail=1; }
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "NO DHCP LEASE" \
  && echo "PASS: summary outcome (no lease)" || { echo "FAIL: summary outcome"; fail=1; }
# Recommendation block: the skipped-quarantined note names Nic2 + points at --clear.
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "skipped.*quarantined.*Nic2\.efi|Nic2\.efi.*quarantined" \
  && echo "PASS: summary recommends on quarantined driver" || { echo "FAIL: summary quarantine rec"; fail=1; }
sect MARK_SUM_AUTO MARK_SUM_DUMP | grep -aqiE "netload --clear|--clear" \
  && echo "PASS: summary points at --clear" || { echo "FAIL: summary --clear hint"; fail=1; }
# Persisted: a one-line SWEEP result must survive into the NVRAM log for --dump.
sect MARK_SUM_DUMP MARK_DONE | grep -aqE "SWEEP" \
  && echo "PASS: sweep result persisted to NVRAM log" || { echo "FAIL: NVRAM SWEEP line"; fail=1; }
# --dump reconstructs a readable findings table from the NVRAM log ring, so a
# crashed/hung sweep still yields the summary. Rendered labels ("no NIC",
# "CRASHED") are distinct from the raw log tokens ("NONIC", "CRASH").
sect MARK_SUM_DUMP MARK_DONE | grep -aqiE "reconstructed" \
  && echo "PASS: --dump reconstructs a findings table" || { echo "FAIL: dump table header"; fail=1; }
sect MARK_SUM_DUMP MARK_DONE | grep -aqiE "no NIC" \
  && echo "PASS: --dump table renders no-NIC outcome" || { echo "FAIL: dump table no-NIC"; fail=1; }
sect MARK_SUM_DUMP MARK_DONE | grep -aqiE "Nic2\.efi.*CRASHED|CRASHED.*Nic2\.efi" \
  && echo "PASS: --dump table renders the crashed driver" || { echo "FAIL: dump table crashed"; fail=1; }
# --- Config validation guards (netload_cfg_parse) ---
sect MARK_ERR_IP_NOSEL MARK_ERR_MACNIC | grep -aqiE "static IP needs a target NIC|--mac|--nic" \
  && echo "PASS: --ip without selector under -a errors" || { echo "FAIL: ip-nosel guard"; fail=1; }
sect MARK_ERR_MACNIC MARK_ERR_BADIP | grep -aqiE "mutually exclusive|--mac and --nic" \
  && echo "PASS: --mac + --nic mutually exclusive" || { echo "FAIL: mac/nic exclusive"; fail=1; }
sect MARK_ERR_BADIP MARK_ERR_MASK | grep -aqiE "invalid.*ip|could not parse|999" \
  && echo "PASS: malformed --ip rejected" || { echo "FAIL: bad ip"; fail=1; }
sect MARK_ERR_MASK MARK_ERR_GW | grep -aqiE "invalid.*mask" \
  && echo "PASS: malformed --mask rejected" || { echo "FAIL: mask"; fail=1; }
sect MARK_ERR_GW MARK_ERR_DNS | grep -aqiE "invalid.*gw" \
  && echo "PASS: malformed --gw rejected" || { echo "FAIL: gw"; fail=1; }
sect MARK_ERR_DNS MARK_ERR_DT | grep -aqiE "invalid.*dns" \
  && echo "PASS: malformed --dns rejected" || { echo "FAIL: dns"; fail=1; }
# --dhcp-timeout/--retries/--nic are AXL_ARG_U32 (axl-args.h .min/.max), so
# these three are now rejected by axl_args_run itself before netload_cfg_parse
# ever runs -- the framework's own "not a valid integer" / "is below min" /
# "exceeds the type's range" wording, not netload's former hand-rolled text.
sect MARK_ERR_DT MARK_ERR_RT | grep -aqiE "dhcp-timeout.*not a valid integer" \
  && echo "PASS: malformed --dhcp-timeout rejected" || { echo "FAIL: dt"; fail=1; }
sect MARK_ERR_RT MARK_ERR_OVERFLOW | grep -aqiE "retries.*below min" \
  && echo "PASS: --retries 0 rejected" || { echo "FAIL: rt"; fail=1; }
sect MARK_ERR_OVERFLOW MARK_HELP_SHORT | grep -aqiE "nic.*exceeds" \
  && echo "PASS: --nic overflow (2^32+1) rejected, not silently wrapped" \
  || { echo "FAIL: nic overflow wraps silently"; fail=1; }
sect MARK_HELP_SHORT MARK_DONE | grep -aqiE "netload" \
  && echo "PASS: -h short help runs" || { echo "FAIL: -h"; fail=1; }
# Hidden test/diagnostic seams (.hidden) must NOT appear in --help / -h output,
# and the old "TEST SEAM:" reader-warning crutch must be gone. Real flags stay.
leak=0
for hs in _mark _hmap _saveconf _applydry _drvresolve "TEST SEAM" "DIAG SEAM"; do
  if sect MARK_HELP MARK_CLR | grep -aqF -- "$hs" \
     || sect MARK_HELP_SHORT MARK_NOREACH_SEED | grep -aqF -- "$hs"; then
    echo "FAIL: help leaks hidden seam '$hs'"; fail=1; leak=1
  fi
done
[[ "$leak" == 0 ]] && echo "PASS: --help/-h omit all hidden test seams"
sect MARK_HELP MARK_CLR | grep -aqF -- "--auto" \
  && echo "PASS: --help still lists real flags (--auto)" \
  || { echo "FAIL: --help dropped real flags"; fail=1; }
# --- PR_NO_REACH rendering (--dump reconstructs "up, no reach" from NOREACH) ---
# "up, no reach" appears ONLY via log_token_label's NOREACH mapping -- the raw
# log line is "NOREACH Rtk.efi", so this pins the label mapping, not the echo.
sect MARK_NOREACH_DUMP MARK_DONE | grep -aq "up, no reach" \
  && echo "PASS: NOREACH renders in --dump" || { echo "FAIL: noreach dump"; fail=1; }
# --- Config save/apply round-trip (structural) ---
# Pin every key=value line the --_saveconf calls above wrote. AxlConfigFile
# entry order is unspecified (it's a hash-table map), so this checks each
# key present rather than one exact delimited line -- the honest test for a
# contract that explicitly documents unordered serialization.
sect MARK_SAVE_DUMP MARK_APPLYDRY | grep -aqF "driver=Rtk.efi" \
  && sect MARK_SAVE_DUMP MARK_APPLYDRY | grep -aqF "method=static" \
  && sect MARK_SAVE_DUMP MARK_APPLYDRY | grep -aqF "ip=10.0.0.5" \
  && sect MARK_SAVE_DUMP MARK_APPLYDRY | grep -aqF "mask=255.255.255.0" \
  && sect MARK_SAVE_DUMP MARK_APPLYDRY | grep -aqF "gw=10.0.0.1" \
  && sect MARK_SAVE_DUMP MARK_APPLYDRY | grep -aqF "mac=60:7d:09:57:f8:bf" \
  && echo "PASS: saved Config shown in --dump" || { echo "FAIL: config dump"; fail=1; }
sect MARK_SAVE_GONE MARK_BADCFG_SEED | grep -aqiE "10\.0\.0\.5" \
  && { echo "FAIL: Config survived --clear"; fail=1; } || echo "PASS: --clear removes Config"
# --- config_load() unit coverage via --_applydry (bucket B: EXACT parsed fields).
# The valid line seeded above must round-trip through the tokenizer/validator;
# these pin the exact reconstructed field strings (not a substring of the raw
# line), so a parse regression that mangles a field is caught.
sect MARK_APPLYDRY MARK_SAVE_CLR | grep -aqE "applydry: driver=Rtk\.efi method=static ip=10\.0\.0\.5 mask=255\.255\.255\.0 gw=10\.0\.0\.1 dns=0 mac=60:7d:09:57:f8:bf" \
  && echo "PASS: config_load parses a valid line exactly" || { echo "FAIL: applydry parse"; fail=1; }
# A garbage line has no '=' anywhere -> parses to an empty map -> config_load must reject it.
sect MARK_BADCFG_DRY MARK_BADCFG_CLR | grep -aqE "applydry: malformed" \
  && echo "PASS: config_load rejects a malformed line" || { echo "FAIL: applydry malformed"; fail=1; }
# --_fwrow: the firmware-row detail wording. Exact strings (not substrings): a
# regression to "no firmware NIC bound" for N>0 -- the real Dell R6725 symptom
# where 4 link-down Broadcoms were enumerated -- must be caught. N>0 says the
# NICs are present-but-down; N=0 keeps the genuinely-absent wording.
sect MARK_FWROW_DOWN MARK_FWROW_DOWN_DONE | grep -aqF "4 firmware NICs present, all link-down" \
  && echo "PASS: firmware row says NICs present-but-down (not 'none bound')" \
  || { echo "FAIL: firmware-row down wording"; fail=1; }
sect MARK_FWROW_NONE MARK_FWROW_NONE_DONE | grep -aqF "no firmware NIC bound" \
  && echo "PASS: firmware row says 'no firmware NIC bound' when truly none present" \
  || { echo "FAIL: firmware-row none wording"; fail=1; }
# --- --json machine-readable result object ---
# Aaa.efi is a dummy .efi (hexdump.efi) that loads but never produces a NIC,
# so its result is "none" -- this only pins that the object is emitted (with
# the right driver name) even on a non-win, and that progress logging is not
# suppressed. A real leased/ping rtt value is HW-only (no NIC in this QEMU
# boot); see test-meta header.
sect MARK_JSON MARK_JSONQ_CLR | grep -aqE '\{"driver":"Aaa\.efi"' \
  && echo "PASS: --json emits a result object" || { echo "FAIL: json object"; fail=1; }
sect MARK_JSON MARK_JSONQ_CLR | grep -aqiE "loading Aaa\.efi" \
  && echo "PASS: --json keeps progress logs" || { echo "FAIL: json progress"; fail=1; }
# --json escaping regression: a driver name containing a raw
# backslash must come out of AxlJsonWriter escaped ("\\") in the "driver"
# field. A hand-rolled predecessor of print_json_result interpolated the CLI
# string unescaped -- "\B" is not a legal JSON escape, so this exact line used
# to be unparsable. print_json_result's own AxlJsonReader round-trip
# (json_result_selfcheck) re-parses the line it just built and would print an
# "INTERNAL ERROR" line if the escaping ever regresses -- that is the reader-
# based proof of validity, not just the eyeballed escaped substring below.
sect MARK_JSON_ESC MARK_JSONQ_CLR | grep -aqF '"driver":"A\\B.efi"' \
  && echo "PASS: --json escapes a raw backslash in the driver field" \
  || { echo "FAIL: json backslash escaping"; fail=1; }
sect MARK_JSON_ESC MARK_JSONQ_CLR | grep -aqE "INTERNAL ERROR" \
  && { echo "FAIL: json self-check (AxlJsonReader round-trip) flagged the escaped line"; fail=1; } \
  || echo "PASS: AxlJsonReader round-trips the escaped driver field cleanly"
# A no-win -a --json sweep whose last candidate is quarantined must still emit
# exactly one JSON line, and must NOT falsely claim "result":"up" for the
# skipped representative row.
sect MARK_JSONQ MARK_DONE | grep -aqE '\{"driver":' \
  && echo "PASS: no-win quarantined sweep still emits a JSON line" || { echo "FAIL: json line on quarantined sweep"; fail=1; }
sect MARK_JSONQ MARK_DONE | grep -aqE '"result":"up"' \
  && { echo "FAIL: false result:up on quarantined sweep"; fail=1; } || echo "PASS: no false result:up"
# --- Output tee (-o/--out), --diag full dump, drivers-in-dump ---
# `type nltee.txt` after `--list -o nltee.txt` must echo the tee'd list content.
sect MARK_OUT_TYPE MARK_DIAG | grep -aqE "Aaa\.efi" \
  && echo "PASS: --out tee wrote netload output to a file" || { echo "FAIL: --out tee"; fail=1; }
# --diag prints netload's own network-landscape section + the shell 'drivers'
# dump via EFI_SHELL_PROTOCOL.Execute (the unavailable fallback must NOT appear).
sect MARK_DIAG MARK_DGOUT | grep -aqiE "network interfaces" \
  && echo "PASS: --diag prints the network landscape" || { echo "FAIL: --diag landscape"; fail=1; }
sect MARK_DIAG MARK_DGOUT | grep -aqE "=== shell: drivers ===" \
  && echo "PASS: --diag ran the shell 'drivers' dump" || { echo "FAIL: --diag drivers"; fail=1; }
sect MARK_DIAG MARK_DGOUT | grep -aqiE "shell command unavailable" \
  && { echo "FAIL: --diag shell Execute unavailable"; fail=1; } || echo "PASS: --diag shell Execute worked"
# Default --diag must NOT run 'dh -v' (only the --dh opt-in does).
sect MARK_DIAG MARK_DGOUT | grep -aqE "shell: dh -v" \
  && { echo "FAIL: default --diag ran dh -v (must be --dh opt-in)"; fail=1; } || echo "PASS: default --diag omits dh -v"
# --diag -o FILE tees the landscape AND appends 'drivers' to the file (the path
# whose absence let the Dell RSOD slip in).
sect MARK_DGTYPE MARK_DGDH | grep -aqiE "network interfaces" \
  && echo "PASS: --diag -o tees the landscape to a file" || { echo "FAIL: --diag -o landscape in file"; fail=1; }
sect MARK_DGTYPE MARK_DGDH | grep -aqE "=== shell: drivers ===" \
  && echo "PASS: --diag -o appends 'drivers' to the file" || { echo "FAIL: --diag -o drivers in file"; fail=1; }
# --dh opt-in runs 'dh -v' to screen with the 'screen only' safety note.
sect MARK_DGDH MARK_DGAUTO | grep -aqiE "dh -v .screen only" \
  && echo "PASS: --dh runs dh -v to screen with the safety note" || { echo "FAIL: --dh dh-v note"; fail=1; }
# -a -d composes: the auto sweep AND the diagnostic report both run (diag no
# longer preempts the sweep -- the bug Mike hit: `-a -d` did no DHCP sweep).
sect MARK_DGAUTO MARK_DUMP_DRV | grep -aqiE "netload auto sweep" \
  && echo "PASS: -a -d still runs the auto sweep" || { echo "FAIL: -a -d sweep preempted"; fail=1; }
sect MARK_DGAUTO MARK_DUMP_DRV | grep -aqE "=== netload diagnostic dump ===" \
  && echo "PASS: -a -d appends the diagnostic report after the sweep" || { echo "FAIL: -a -d diag missing"; fail=1; }
# --dump appends the shell 'drivers' list after the findings.
sect MARK_DUMP_DRV MARK_DONE | grep -aqE "=== shell: drivers ===" \
  && echo "PASS: --dump includes the drivers list" || { echo "FAIL: --dump drivers"; fail=1; }
# --- Firmware-first end-to-end + recursive-connect regression guard ---
# A SEPARATE --net boot (so the firmware NIC doesn't short-circuit the no-net
# sweeps above): QEMU's OVMF ships the whole network stack incl. VirtioNetDxe.
# `netload --connect` must connect the firmware NIC and DHCP it. Getting a lease
# requires axl_driver_connect(NULL)'s RECURSIVE `connect -r` to cascade the full
# stack (virtio-net -> SnpDxe -> MnpDxe -> ArpDxe -> Ip4Dxe -> Dhcp4Dxe); if the
# Recursive flag were ever dropped, the cascade breaks and no lease is acquired.
# This is also the answer to "will -a lease via a deeply-nested built-in NIC
# driver?" -- yes, the firmware-first recursive connect resolves arbitrary depth.
FWLOG="$TMP/fw.log"; FWNSH="$TMP/fw.nsh"
{ echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_FWNET'
  echo 'netload.efi --connect'; echo 'echo MARK_FWNET_DONE'
  # -a with no staged drivers present (no --dir / no --extra on this image):
  # the firmware-first probe still runs first and, on this --net boot, still
  # wins -- this is the reports[0] regression guard: the -a summary must
  # show a firmware-tagged row (and print at all) even though it never
  # reaches the (empty) staged sweep.
  echo 'echo MARK_FWAUTO'; echo 'netload.efi -a'; echo 'echo MARK_FWAUTO_DONE'
  echo 'echo MARK_FWAUTO_JSON'; echo 'netload.efi -a --json'; echo 'echo MARK_FWAUTO_JSON_DONE'
  # --_drvresolve diagnostic seam: dumps how each bound NIC's driver resolves,
  # with the library DEBUG resolution trace surfaced. Smoke-check only here (the
  # real use is pinning a real-HW "<firmware volume>"); assert it runs, sees the
  # firmware NIC, and emits a resolution trace line.
  echo 'echo MARK_DRVRES'; echo 'netload.efi --_drvresolve'; echo 'echo MARK_DRVRES_DONE'
  echo 'reset -s'; } > "$FWNSH"
"$DIR/scripts/run-qemu.sh" --arch "$ARCH" --net --timeout 150 --nsh "$FWNSH" \
  "$TOOLS/netload.efi" > "$FWLOG" 2>&1 || true
fsect() { sed -n "/$1/,/$2/p" "$FWLOG"; }
fsect MARK_FWNET MARK_FWNET_DONE | grep -aqiE "link=UP" \
  && echo "PASS: firmware-first sees the firmware NIC (recursive connect bound it)" \
  || { echo "FAIL: firmware NIC not seen under --net"; fail=1; }
# --_drvresolve smoke: seam runs, enumerates the NIC, emits the DEBUG trace.
fsect MARK_DRVRES MARK_DRVRES_DONE | grep -aqE "driver resolution for [0-9]+ NIC" \
  && fsect MARK_DRVRES MARK_DRVRES_DONE | grep -aqE "resolve: layer=.*source=" \
  && echo "PASS: --_drvresolve emits a resolution trace" \
  || { echo "FAIL: --_drvresolve trace"; fail=1; }
fsect MARK_FWNET MARK_FWNET_DONE | grep -aqiE "networking is UP via a firmware driver" \
  && echo "PASS: firmware-first leased via the firmware stack (recursive cascade holds)" \
  || { echo "FAIL: firmware-first got no lease under --net"; fail=1; }
# The bound driver's NAME must resolve (ComponentName2 fallback) for a
# firmware-dispatched NIC -- it has no .efi filename, so the old FilePath-only
# walk returned the "<firmware volume>"/"<unknown>" placeholder.
fsect MARK_FWNET MARK_FWNET_DONE | grep -aqiE "driver=<unknown>|driver=<firmware volume>" \
  && { echo "FAIL: firmware NIC driver name unresolved (ComponentName2 not consulted)"; fail=1; } \
  || echo "PASS: firmware NIC driver name resolved via ComponentName2 (not a placeholder)"
# --- reports[0]: the firmware-first result belongs in the -a summary, always ---
# Under --net the firmware NIC leases via SLIRP DHCP (proved above), so -a's
# firmware-first probe WINS here -- the regression this guards is cmd_auto
# returning 0 on that win WITHOUT ever printing a summary (so an operator
# could not tell the built-in path was even tried). The row must be tagged
# distinctly ("firmware:LEASED") and the bottom-line outcome must still print.
fsect MARK_FWAUTO MARK_FWAUTO_DONE | grep -aqiE "firmware:LEASED" \
  && echo "PASS: -a's summary shows a firmware-tagged LEASED row (reports[0])" \
  || { echo "FAIL: no firmware row in the -a summary on a firmware win"; fail=1; }
fsect MARK_FWAUTO MARK_FWAUTO_DONE | grep -aqiE "NETWORKING IS UP" \
  && echo "PASS: -a still prints the summary on a firmware win (no early silent return)" \
  || { echo "FAIL: -a summary not printed on a firmware win"; fail=1; }
# --json must also emit an object on a firmware win (the same gap, --json side).
fsect MARK_FWAUTO_JSON MARK_FWAUTO_JSON_DONE | grep -aqE '"result":"up"' \
  && echo "PASS: -a --json emits a result object on a firmware win" \
  || { echo "FAIL: -a --json emitted nothing on a firmware win"; fail=1; }

# --- Multi-NIC IP4Config2 index bug (real-HW: a link-up NIC never leases) ---
# TWO NICs; only the SECOND (MAC ...57, a HIGH SimpleNetwork index -- it sits
# behind the first NIC's SNP child handles) is on a DHCP netdev. Pre-fix,
# axl_net_auto_init indexed the IP4Config2 handle array with the SNP-derived
# nic_index; for the high index that clamped to handle 0 (the OTHER NIC), so
# DHCP was applied to the wrong NIC and the DHCP-capable NIC never leased --
# exactly the Dell R6725 symptom (Rtk NIC link-up, no lease). The fix resolves
# IP4Config2 through the NIC registry, correlating each physical NIC to its
# IP4Config2 handle BY MAC. Guard: selecting ...57 must LEASE.
# net A (...56) is a dead socket netdev (NO DHCP), so the buggy "DHCP to handle
# 0" path yields no lease; a false pass is impossible. The listen port is
# claimed from the shared allocator rather than pinned per-arch: a fixed port
# only keeps the x64/aa64 pair of ONE run apart, and two independent runs of
# the suite collide on it.
axl_alloc_host_port IDXPORT || exit 1
IDXLOG="$TMP/idx.log"; IDXNSH="$TMP/idx.nsh"
{ echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_IDX'
  echo 'netload.efi --connect --mac 52:54:00:12:34:57'; echo 'echo MARK_IDX_DONE'
  echo 'echo MARK_IDX_LIST'; echo 'netload.efi --diag'; echo 'echo MARK_IDX_LIST_DONE'
  echo 'reset -s'; } > "$IDXNSH"
# virtio-net-pci (native VirtioNetDxe on BOTH arches -- unlike e1000, whose
# iPXE UNDI option ROM is x86-only). The SNP child-handle dupes that cause the
# index divergence come from OVMF's SNP/MNP layering, not the NIC model, so
# virtio reproduces it too. Explicit MACs pin which NIC is the high index.
"$DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 120 --nsh "$IDXNSH" \
  --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=nA,mac=52:54:00:12:34:56" \
  --qemu-arg -netdev --qemu-arg "socket,id=nA,listen=127.0.0.1:${IDXPORT}" \
  --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=nB,mac=52:54:00:12:34:57" \
  --qemu-arg -netdev --qemu-arg user,id=nB \
  "$TOOLS/netload.efi" > "$IDXLOG" 2>&1 || true
isect() { sed -n "/$1/,/$2/p" "$IDXLOG"; }
isect MARK_IDX MARK_IDX_DONE | grep -aqiE "networking is UP via a firmware driver" \
  && echo "PASS: multi-NIC IP4Config2 resolved by MAC -- the high-index DHCP NIC leases" \
  || { echo "FAIL: multi-NIC IP4Config2 index bug -- high-index NIC did not lease"; fail=1; }

# Dedup guard: TWO virtio NICs must list as TWO interfaces. print_net_landscape
# renders each row as "  NIC <mac>  link=<UP|DOWN>  layer=...  driver=...".
# print_net_landscape no longer de-dupes by MAC client-side -- that workaround
# (mac_in() over a local seen[] array) was removed once axl_net_list_interfaces
# itself returned one row per physical NIC. So this assertion is now real,
# independent proof of the registry's dedup on the operator-facing --diag
# landscape: if the library ever regresses to one row per SNP child handle,
# this reports 6 (or some other wrong count), not 2, and fails.
IDXN=$(isect MARK_IDX_LIST MARK_IDX_LIST_DONE | grep -acE '^ *NIC [0-9A-Fa-f:]+  link=(UP|DOWN)' || true)
[[ "$IDXN" -eq 2 ]] \
  && echo "PASS: 2 NICs list as 2 interfaces (SNP child handles deduped)" \
  || { echo "FAIL: expected 2 interfaces, got $IDXN (SNP child dupes leaking?)"; fail=1; }

# --- axl_net_bring_up addr_out must describe the NIC it CONFIGURED (BUG B) ---
# The wrong-answer bug this guards: bring_up used to fill addr_out by calling
# axl_net_get_ip_address, which is NIC-agnostic BY DESIGN ("does ANY NIC have an
# address" -- first configured IP4Config2 wins). On a multi-NIC box that can
# name a DIFFERENT NIC than the one just configured, and netload reports that
# address to the operator verbatim.
#
# Two NICs, each given a DIFFERENT, known static address, so a mix-up shows up
# as a literal wrong value:
#   1. NIC A (...56) -> 192.168.99.1
#   2. NIC B (...57) -> 192.168.88.1     <- the discriminating step
#
# ORDER IS LOAD-BEARING, and was determined empirically, not assumed. NIC A's
# IP4Config2 handle enumerates FIRST, so get_ip_address ("first configured
# IP4Config2 wins") answers with A's address. Configuring B while A already has
# one is therefore the only arrangement where the NIC-agnostic reader returns
# the WRONG NIC: the reverse order (configure A while B is up) passes even
# against the pre-fix library, because "first configured" happens to BE the NIC
# under configuration -- right answer, wrong reason. That variant was tried
# first and did not discriminate; this one does.
#
# This ordering is OVMF behavior, not a spec guarantee -- if it ever flips,
# the checks below would silently stop discriminating. A mechanical guard
# (the "BUG B precondition" check after the decisive assertions, fed by
# --_hmap) pins the expected ip4cfg[] order and fails loudly, distinctly from
# a bring_up regression, if OVMF's enumeration ever changes.
#
# Both steps are STATIC, which isolates addr_out precisely: the static path
# never calls axl_net_auto_init, so the short-circuit bug (the OTHER half of
# this fix) cannot contaminate the result. The only thing under test is which
# NIC addr_out describes. A static address needs no reachability, so A's dead
# socket netdev is irrelevant.
#
# What this does NOT guard: the transient "configured but the address has not
# settled yet" case (set_static_ip drops the old address before the new one
# lands). Under QEMU the 500 ms settle is always sufficient, so has_ipv4 is true
# by read-back time and this cannot exercise the tri-state's
# attributable-but-no-address-yet branch. That branch is real-firmware territory.
axl_alloc_host_port BUGBPORT || exit 1
BUGBLOG="$TMP/bugb.log"; BUGBNSH="$TMP/bugb.nsh"
{ echo '@echo -off'; echo 'fs0:'
  echo 'echo MARK_BUGB_FIRST'
  echo 'netload.efi --connect --mac 52:54:00:12:34:56 --ip 192.168.99.1'
  echo 'echo MARK_BUGB_FIRST_DONE'
  echo 'echo MARK_BUGB_SECOND'
  echo 'netload.efi --connect --mac 52:54:00:12:34:57 --ip 192.168.88.1'
  echo 'echo MARK_BUGB_SECOND_DONE'
  # Precondition check for the discriminating step above: --_hmap dumps the
  # firmware's IP4Config2 handle enumeration order (see dump_hmap in
  # netload.c). Ridden into this SAME boot -- no new boot needed -- because
  # the order is a property of firmware handle-database order (LocateHandleBuffer),
  # not of which NIC was just configured, so it's valid to sample it here.
  echo 'echo MARK_BUGB_HMAP'; echo 'netload.efi --_hmap'
  echo 'echo MARK_BUGB_HMAP_DONE'
  echo 'reset -s'; } > "$BUGBNSH"
"$DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 120 --nsh "$BUGBNSH" \
  --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=nA,mac=52:54:00:12:34:56" \
  --qemu-arg -netdev --qemu-arg "socket,id=nA,listen=127.0.0.1:${BUGBPORT}" \
  --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=nB,mac=52:54:00:12:34:57" \
  --qemu-arg -netdev --qemu-arg user,id=nB \
  "$TOOLS/netload.efi" > "$BUGBLOG" 2>&1 || true
bsect() { sed -n "/$1/,/$2/p" "$BUGBLOG"; }
# Precondition: NIC A really took its address, so there IS a foreign address for
# step 2 to wrongly report. Without this the decisive checks pass vacuously.
bsect MARK_BUGB_FIRST MARK_BUGB_FIRST_DONE | grep -aqE "address up: 192\.168\.99\.1" \
  && echo "PASS: BUG B setup: NIC ...56 configured 192.168.99.1 (the foreign address)" \
  || { echo "FAIL: BUG B setup -- NIC ...56 not configured (test cannot discriminate)"; fail=1; }
# THE decisive assertion: configure B, report B's OWN address -- never A's.
bsect MARK_BUGB_SECOND MARK_BUGB_SECOND_DONE | grep -aqE "address up: 192\.168\.88\.1" \
  && echo "PASS: bring_up addr_out reports the NIC it configured (192.168.88.1)" \
  || { echo "FAIL: BUG B -- addr_out did not report the configured NIC's own address"; fail=1; }
bsect MARK_BUGB_SECOND MARK_BUGB_SECOND_DONE | grep -aqE "address up: 192\.168\.99\.1" \
  && { echo "FAIL: BUG B -- addr_out reported the OTHER NIC's address (wrong NIC)"; fail=1; } \
  || echo "PASS: addr_out never reports the other NIC's address"
# Mechanically detect the precondition the three checks above silently depend
# on: BUG B only discriminates because OVMF's IP4Config2 handle enumeration
# puts NIC A (...56, left holding the foreign address) BEFORE NIC B (...57,
# the one actually configured in the decisive step) -- see the "ORDER IS
# LOAD-BEARING" comment above. If that firmware ordering ever flips, the
# three checks above would keep passing for the WRONG reason (or, in the
# reversed arrangement, pass vacuously the way the first draft of this test
# did). This reads --_hmap's own "ip4-config2 handles ... idx order" dump
# (tools/netload.c dump_hmap) and pins ip4cfg[0]/ip4cfg[1] to the exact MACs
# this test's discrimination requires, so a flip fails HERE, distinctly from
# a BUG B regression, instead of the suite quietly losing coverage.
hm=$(bsect MARK_BUGB_HMAP MARK_BUGB_HMAP_DONE)
ip4_0=$(printf '%s\n' "$hm" | grep -aE "ip4cfg\[0\]" | grep -oE '[0-9a-f]{2}(:[0-9a-f]{2}){5}' || true)
ip4_1=$(printf '%s\n' "$hm" | grep -aE "ip4cfg\[1\]" | grep -oE '[0-9a-f]{2}(:[0-9a-f]{2}){5}' || true)
if [[ "$ip4_0" == "52:54:00:12:34:56" && "$ip4_1" == "52:54:00:12:34:57" ]]; then
  echo "PASS: BUG B precondition holds -- IP4Config2 still enumerates ...56 (foreign-address NIC) before ...57 (configured NIC), so the checks above actually discriminate"
else
  echo "FAIL: BUG B TEST PRECONDITION BROKEN (NOT a bring_up regression) -- IP4Config2 enumeration order changed: expected ip4cfg[0]=52:54:00:12:34:56 ip4cfg[1]=52:54:00:12:34:57, got ip4cfg[0]=${ip4_0:-<none>} ip4cfg[1]=${ip4_1:-<none>}. The three BUG B checks above no longer prove anything -- this test needs to be re-derived for the new firmware order (see the 'ORDER IS LOAD-BEARING' comment above)."
  fail=1
fi

# ======================================================================
# Link-down NIC tests: AUTO link-up preference + firmware-all-down wording
# ======================================================================
# A guest-visible link-down virtio NIC was impossible in QEMU until we learned
# TWO things (see local/docs/handoff-qemu-linkdown-optionA.md):
#
#   (1) `romfile=` (empty) is LOAD-BEARING, not decoration. QEMU ships an iPXE
#       UNDI option ROM for virtio-net (1af4:1000). When present OVMF binds the
#       iPXE UNDI -- an NII3.1 SNP producer whose MediaPresent does NOT track
#       virtio link status (reports UP regardless). `romfile=` disables that ROM
#       so OVMF's native VirtioNetDxe, which DOES read virtio config.status into
#       SNP MediaPresent, is the SNP producer. Without `romfile=` a link-down NIC
#       still reads UP and every link-down test here is a silent no-op. This is
#       why the prior session's "set_link off doesn't work" experiments (all run
#       WITHOUT romfile=) were dead ends -- they were reading the iPXE UNDI.
#
#   (2) Two ways to actually put that native NIC's link down, applied
#       BELT-AND-SUSPENDERS so these tests run whether or not the QEMU patch is
#       present (CI / another dev runs stock QEMU):
#         a. `x-link-down=on` -- a virtio-net device property added by a LOCAL
#            patch to ~/projects/qemu/qemu-10.0.0. Realizes the NIC with virtio
#            config.status LINK_UP clear, so OVMF reads MediaPresent=FALSE at
#            bind. Deterministic, no timing, no host tooling -- but the property
#            only exists on the patched QEMU (stock QEMU ABORTS on an unknown
#            property, so it is applied ONLY when probed-present, never blindly).
#         b. QMP `set_link <netdev> off` -- works on STOCK QEMU. Driven early and
#            continuously (before OVMF binds the NIC) so the NIC is down by the
#            time VirtioNetDxe reads it; OVMF's MNP background poll also refreshes
#            MediaPresent live, so the registry (which reads Mode->MediaPresent
#            without its own GetStatus) still sees DOWN. Needs `socat`.
#       On the patched dev box BOTH apply (x-link-down does it; set_link is
#       redundant insurance). On stock QEMU set_link alone does it. If NEITHER is
#       available (unpatched QEMU AND no socat) the two tests SKIP, loudly, rather
#       than fail -- the netload logic they guard is otherwise real-hardware-only.
LD_QEMU=$(find_qemu "$ARCH" 2>/dev/null || true)
LD_HAVE_XLD=0
if [[ -n "$LD_QEMU" ]] && "$LD_QEMU" -device virtio-net-pci,help 2>&1 | grep -q 'x-link-down'; then
  LD_HAVE_XLD=1
fi
LD_HAVE_SOCAT=0; command -v socat >/dev/null 2>&1 && LD_HAVE_SOCAT=1
# `x-link-down=on,` prefix for a link-down NIC's -device string (empty on stock).
LD_XLD=""; [[ "$LD_HAVE_XLD" -eq 1 ]] && LD_XLD="x-link-down=on,"

# QMP injector: drive `set_link off` on the named netdev(s) until killed. Written
# once, reused by both tests. Self-contained (own qmp_capabilities handshake per
# connect); errors ignored so it never disturbs the run if QMP is momentarily
# unavailable. Continuous 1 Hz loop guarantees it lands before OVMF binds the NIC
# regardless of host boot speed. Started before run-qemu, killed (own captured
# PID + children only -- never `pkill -f`, which once killed a colleague's
# softbmc) after.
cat > "$TMP/setlink-off.sh" <<'INJECTOR'
#!/bin/bash
sock="$1"; shift; nets=("$@")
for _ in $(seq 1 120); do [[ -S "$sock" ]] && break; sleep 0.25; done
# Bounded loop (~300s), NOT `while :`. ld_inject runs us inside a `$(...)`
# subshell, so we are reparented to init if the test dies before ld_inject_stop
# kills us -- an unbounded loop would then spin forever in CI. 300 iterations far
# outlast any single boot (run-qemu --timeout 120) yet guarantee self-exit.
for _ in $(seq 1 300); do
  { printf '{"execute":"qmp_capabilities"}\n'; sleep 0.2
    for n in "${nets[@]}"; do
      printf '{"execute":"set_link","arguments":{"name":"%s","up":false}}\n' "$n"; sleep 0.1
    done
  } | socat -t 3 - "UNIX-CONNECT:$sock" >/dev/null 2>&1 || true
  sleep 1
done
INJECTOR
chmod +x "$TMP/setlink-off.sh"
# ld_inject <sock> <netdev...>  -> echoes injector PID ("" if socat absent).
# The background child's fds MUST be redirected off the command-substitution
# pipe: an inherited stdout keeps `$(...)` blocked until the (forever-looping)
# child closes it -- i.e. hangs the whole test. </dev/null in too, so it never
# competes for the run's stdin.
ld_inject() {
  [[ "$LD_HAVE_SOCAT" -eq 1 ]] || { echo ""; return 0; }
  "$TMP/setlink-off.sh" "$@" </dev/null >/dev/null 2>&1 & echo "$!"
}
ld_inject_stop() {  # <pid>
  [[ -n "$1" ]] || return 0
  pkill -P "$1" 2>/dev/null || true
  kill "$1" 2>/dev/null || true
  wait "$1" 2>/dev/null || true
}

if [[ "$LD_HAVE_XLD" -eq 0 && "$LD_HAVE_SOCAT" -eq 0 ]]; then
  echo "SKIP: link-down tests need the QEMU x-link-down patch OR socat for QMP set_link (have neither) -- see local/docs/handoff-qemu-linkdown-optionA.md"
  echo "SKIP: AUTO link-up preference (no link-down mechanism available)"
  echo "SKIP: firmware-all-down wording (no link-down mechanism available)"
else
  # --- AUTO link-up preference: a link-DOWN low-index NIC must not divert bring-up
  # (the Dell CFMNTD1 scenario, previously QEMU-untestable) ---
  # NIC-0 (...aa) boots link-DOWN on subnet 10.10.0.x; NIC-1 (...bb) boots link-UP
  # on a DISTINCT subnet 10.20.0.x. probe_firmware_stack (netload.c) skips a
  # link-down NIC (`!ls.link_up -> continue`) and brings up the link-up one, so
  # the leased address MUST be NIC-1's 10.20.0.15. The distinct subnets are the
  # discriminator: were the link-up filter (or QEMU's link-down realization)
  # broken, NIC-0 would read UP, be tried first, and -- could it lease -- hand
  # back 10.10.0.15. The printed `link=DOWN`/`link=UP` rows pin the link detection
  # itself; the leased subnet pins that bring-up went via the link-up NIC. Only
  # NIC-0 is knocked down (x-link-down and/or set_link off on n0); NIC-1 stays up.
  LDLOG="$TMP/linkdown.log"; LDNSH="$TMP/linkdown.nsh"; LDSOCK="$TMP/ld-qmp.sock"
  { echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_LD'
    echo 'netload.efi --connect'; echo 'echo MARK_LD_DONE'
    echo 'reset -s'; } > "$LDNSH"
  rm -f "$LDSOCK"; LDINJ=$(ld_inject "$LDSOCK" n0)
  "$DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 120 --nsh "$LDNSH" \
    --qemu-arg -qmp --qemu-arg "unix:$LDSOCK,server,nowait" \
    --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=n0,mac=52:54:00:aa:00:00,${LD_XLD}romfile=" \
    --qemu-arg -netdev --qemu-arg "user,id=n0,net=10.10.0.0/24,dhcpstart=10.10.0.15" \
    --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=n1,mac=52:54:00:bb:00:01,romfile=" \
    --qemu-arg -netdev --qemu-arg "user,id=n1,net=10.20.0.0/24,dhcpstart=10.20.0.15" \
    "$TOOLS/netload.efi" > "$LDLOG" 2>&1 || true
  ld_inject_stop "$LDINJ"
  ldsect() { sed -n "/$1/,/$2/p" "$LDLOG"; }
  # Link detection: the low-index NIC really reads DOWN (the whole point; if it
  # reads UP the link-down mechanism silently failed and the rest is vacuous).
  ldsect MARK_LD MARK_LD_DONE | grep -aqE "NIC 52:54:00:aa:00:00 +link=DOWN" \
    && echo "PASS: link-down NIC reads link=DOWN (native VirtioNetDxe via romfile=)" \
    || { echo "FAIL: link-down NIC did not read DOWN (x-link-down/set_link or romfile= not effective?)"; fail=1; }
  ldsect MARK_LD MARK_LD_DONE | grep -aqE "NIC 52:54:00:bb:00:01 +link=UP" \
    && echo "PASS: link-up NIC reads link=UP" \
    || { echo "FAIL: link-up NIC did not read UP"; fail=1; }
  # Decisive: bring-up leased via the link-UP NIC's subnet, skipping link-down NIC-0.
  ldsect MARK_LD MARK_LD_DONE | grep -aqE "address up: 10\.20\.0\.15" \
    && echo "PASS: AUTO bring-up leased via the link-UP NIC (10.20.0.15), not the link-down low-index NIC" \
    || { echo "FAIL: did not lease the link-up NIC's subnet -- link-down NIC-0 diverted bring-up"; fail=1; }
  ldsect MARK_LD MARK_LD_DONE | grep -aqE "address up: 10\.10\.0\.15" \
    && { echo "FAIL: leased the link-DOWN NIC's subnet (10.10.0.15) -- link filter broken"; fail=1; } \
    || echo "PASS: never leased the link-down NIC's subnet"

  # --- Firmware summary row: "N present, all link-down" via a REAL link-down path ---
  # Same story, exercised end-to-end instead of through the --_fwrow synthetic
  # seam: boot ONLY link-down NICs and confirm probe_firmware_stack's ndown
  # counter and print_row_detail report the honest "N firmware NICs present, all
  # link-down" (commit 634115ff) rather than the misleading "no firmware NIC
  # bound". Two link-down NICs (both knocked down) -> N=2. If link detection
  # regressed (NICs read UP), they would be tried for DHCP and -- on these user
  # netdevs -- LEASE, flipping the outcome away from "all link-down", so the
  # wording assertion also pins the link detection.
  ADLOG="$TMP/alldown.log"; ADNSH="$TMP/alldown.nsh"; ADSOCK="$TMP/ad-qmp.sock"
  { echo '@echo -off'; echo 'fs0:'; echo 'echo MARK_AD'
    echo 'netload.efi -a --dir fs0:\nodir'; echo 'echo MARK_AD_DONE'
    echo 'reset -s'; } > "$ADNSH"
  rm -f "$ADSOCK"; ADINJ=$(ld_inject "$ADSOCK" n0 n1)
  "$DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 120 --nsh "$ADNSH" \
    --qemu-arg -qmp --qemu-arg "unix:$ADSOCK,server,nowait" \
    --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=n0,mac=52:54:00:aa:00:00,${LD_XLD}romfile=" \
    --qemu-arg -netdev --qemu-arg "user,id=n0" \
    --qemu-arg -device --qemu-arg "virtio-net-pci,netdev=n1,mac=52:54:00:aa:00:01,${LD_XLD}romfile=" \
    --qemu-arg -netdev --qemu-arg "user,id=n1" \
    "$TOOLS/netload.efi" > "$ADLOG" 2>&1 || true
  ld_inject_stop "$ADINJ"
  adsect() { sed -n "/$1/,/$2/p" "$ADLOG"; }
  adsect MARK_AD MARK_AD_DONE | grep -aqF "2 firmware NICs present, all link-down" \
    && echo "PASS: firmware summary row reports '2 firmware NICs present, all link-down' (real link-down path, not --_fwrow)" \
    || { echo "FAIL: all-link-down wording missing (expected '2 firmware NICs present, all link-down')"; fail=1; }
  adsect MARK_AD MARK_AD_DONE | grep -aqE "outcome: NO DHCP LEASE" \
    && echo "PASS: all-link-down sweep outcome is NO DHCP LEASE" \
    || { echo "FAIL: all-link-down sweep did not report NO DHCP LEASE (a link-down NIC leased?)"; fail=1; }
fi

[[ "$fail" -eq 0 ]] && { echo "=== PASS ($ARCH) ==="; exit 0; } || { echo "=== FAIL ($ARCH) ==="; exit 1; }
