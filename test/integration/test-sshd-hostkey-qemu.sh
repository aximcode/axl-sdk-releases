#!/bin/bash
# test-meta: arch=both needs=ssh est=60 local-only=0 builds=sshd,sshd-faults guest=none
# guest=none: the boot is part of the subject: the host key must survive the
#   reboot inside its one boot; and it types into the Shell.
# test-sshd-hostkey-qemu.sh -- sshd's persistent host key (AxlSsh P5), graded
# by a real ssh(1) and ssh-keygen(1), in ONE boot that reboots once.
#
# Each step is a Shell script staged on fs0: and started by ONE typed line --
# a command typed while another runs is lost -- which echoes its own markers
# and the exit code of every sshd command in it. Its output lines are read
# whole, between those markers.
#
# The image is sshd-t.efi (the test build: sshd plus the host-key fault
# seams of test/integration/sshd-test-hook.c, armed by files on fs0:),
# staged as sshd.efi. test-efi-pack-release-qemu.sh asserts that the shipped
# sshd.efi links none of those seams.
#
# What it shows, in order:
#   first     `sshd hostkey` with nothing stored; the first start generates and
#             stores a key and says so -- the launcher's line and the driver's
#             two, each whole; status and `sshd hostkey` report it; ssh(1) is
#             shown that same fingerprint; --regenerate and --import are
#             refused while the server runs
#   reboot    the key survives a reboot of the SAME variable store: ssh(1) with
#             StrictHostKeyChecking=yes and the known_hosts entry it recorded
#             is let in. CONTROL: a VOLATILE variable set before the reboot is
#             gone after it, so the reboot really reinitialised the machine
#   regen     `sshd hostkey --regenerate` deletes it, naming the old
#             fingerprint; the next start makes another, and ssh(1) refuses
#             the changed key
#   files     --host-key: an ssh-rsa file is refused by name in sshd's two
#             lines (by the launcher, then by the driver: four on the console)
#             and skipped, the ed25519 file after it serves, and its
#             fingerprint is `ssh-keygen -lf`'s; with only the refused one,
#             "no hostkeys available -- exiting."; `sshd -T` prints one
#             hostkey line per file. A file whose SEED does not derive its
#             public key (lib/hostkey-corrupt-seed.py) passes the launcher's
#             provider-free check, is refused by the DRIVER in sshd's two
#             lines, and the good file after it serves (review M1); `sshd
#             hostkey` names that first readable file and says it is not the
#             full check
#   import    `sshd hostkey --import` refuses to replace a stored key, and with
#             --force stores the file's key, telling the operator to confirm
#             the fingerprint at a start before removing the file
#   denied    a delete the firmware refuses (an authenticated variable, through
#             the seam) names the real recovery, warns of nothing, and leaves
#             the key
#   restore   `--import --force` whose write fails puts the key it replaced
#             back, and says so; when that write fails too, it says that NO
#             key is stored now; and a failed write that stayed in the
#             variable (read-back failed, its delete refused) is named as
#             such -- what is stored is read back before it is reported
#   corrupt   a stored key of the wrong size, one readable by the OS, and one
#             whose seed does not derive its public key (which only the
#             driver can see: the launcher's check passes and the driver's
#             setup refuses it) are each refused and NEVER replaced
#   readerr   a firmware read error is not "absent": refused, nothing written
#             -- by the launcher's pre-check, and (readdxe) by the driver's own
#             backstop when only the driver's read fails
#   degraded  a key that cannot be stored serves anyway, and the service says
#             DEGRADED -- the launcher's line and the driver's ERROR, whole;
#             nothing is stored
#
# THE DRIVER'S LINES are graded whole after their timestamp and level
# (drv_has): `HH:MM:SS.ffffff [LEVEL] sshd: ...`, with whatever the console
# put in front of the timestamp dropped.
#
# SABOTAGED (scripts/sabotage.sh --expect-fail, each caught by the named
# check, x64):
#   - the stored variable written without NV (ATTRS_WANTED BS only):
#     "reboot: the start after it loads the SAME key"
#   - a wrong-size stored value taken as absent, in the library and the
#     launcher: "corrupt: the wrong size is refused by name"
#   - the launcher's second refusal line dropped: "files: the ssh-rsa file is
#     refused by name, in sshd's two lines"
#   - DEGRADED never set: "degraded: status reports the service DEGRADED"
#   - a read error taken as "no key stored": "readerr: a read error is
#     refused by name"
#   - host_key_from_files `return`s at its first failed load instead of
#     going on to the next file: "files: ...and the good file after it
#     serves (exit 0)"
#   - the driver's read-error branch falling through to generation (the
#     library's load-or-create taking AXL_IO_ERROR as absent):
#     "readdxe: the driver's backstop refuses a read error"
#   - verb_hostkey's running refusal dropped: "running: --regenerate and
#     --import are refused while the server runs"
#   - --import --force not putting the old key back: "restore: a failed
#     --force write puts the key it replaced back"
#   - the driver's per-start line printing the wrong source: "files: the
#     driver says it serves the file"
#
# Usage: ./test/integration/test-sshd-hostkey-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
test_setup
source "$(dirname "$0")/lib/sshd-lib.sh"

sshd_require_tools ssh ssh-keygen

declare -A _MK=([X64]=x64 [AARCH64]=aa64)
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_MK[$TEST_ARCH]}" \
        all sshd-faults 2>&1 | tail -2 \
   || [[ ! -f "$(test_build_dir)/sshd-t.efi" ]]; then
    echo "FAIL: sshd-t.efi was not built"
    exit 1
fi
mkdir -p "$TEST_STAGING"
cp "$(test_build_dir)/sshd-t.efi" "$TEST_STAGING/sshd.efi"

# The provisioned keys, made per run by ssh-keygen; only their files go to
# the guest. hk's fingerprint is ssh-keygen's own.
ssh-keygen -q -t ed25519 -N '' -C axl-hk -f "$TEST_TMPDIR/hk" </dev/null
ssh-keygen -q -t rsa -b 1024 -N '' -C '' -f "$TEST_TMPDIR/rsa" </dev/null
ssh-keygen -q -t ed25519 -N '' -C axl-hk2 -f "$TEST_TMPDIR/hk2" </dev/null
cp "$TEST_TMPDIR/hk" "$TEST_STAGING/hk_file"
cp "$TEST_TMPDIR/hk2" "$TEST_STAGING/hk2_file"
cp "$TEST_TMPDIR/rsa" "$TEST_STAGING/rsa_file"
# hk with one seed byte flipped: every provider-free check still passes.
if ! python3 -I "$(dirname "$0")/lib/hostkey-corrupt-seed.py" "$TEST_TMPDIR/hk" \
        "$TEST_STAGING/bad_seed"; then
    echo "FAIL: could not make the corrupt-seed key file"
    exit 1
fi
HK_FP=$(ssh-keygen -lf "$TEST_TMPDIR/hk" | awk '{print $2}')
HK2_FP=$(ssh-keygen -lf "$TEST_TMPDIR/hk2" | awk '{print $2}')
# For an ERE: a fingerprint's '+' is a quantifier there.
HK_FP_RE=${HK_FP//+/[+]}

PW=p5-hostkey-pw
START="sshd.efi -P $PW -x 10.0.2.2/32"
GUID=ef4ea128-0f65-4399-9884-9b0de9f54e9e
VARNAME=AxlSshHostKeyEd25519
VOLGUID=1b2b3c4d-0000-4000-8000-000000000005
KH="$TEST_TMPDIR/known_hosts"
# dmpstore prints a GUID upper-case.
GUID_UC=${GUID^^}
VOLGUID_UC=${VOLGUID^^}
# A well-formed record whose seed does not derive its public key: "AXK", 1,
# the Ed25519 PKCS#8 prefix, a seed of 0x11s and a public key of 0x22s.
BAD="41584b01302e020100300506032b657004220420$(printf '11%.0s' {1..32})$(printf '22%.0s' {1..32})"

# --- the steps, staged as scripts -----------------------------------------------

def() {
    local label="$1" c
    shift
    {
        echo "@echo -off"
        echo "echo HKT-$label-BEGIN"
        for c in "$@"; do
            echo "$c"
            if [[ "$c" == sshd.efi* ]]; then
                echo "echo HKT-$label-RC=%lasterror%"
            fi
        done
        echo "echo HKT-$label-END"
    } > "$TEST_STAGING/$label.nsh"
}
def none      'sshd.efi hostkey'
def start1    "$START"
def status1   'sshd.efi status'
def hk1       'sshd.efi hostkey'
def running   'sshd.efi hostkey --regenerate' 'sshd.efi hostkey --import hk_file'
def vol       'sshd.efi stop' "setvar AxlP5Vol -guid $VOLGUID -bs =01" \
              "dmpstore AxlP5Vol -guid $VOLGUID"
def vol2      "dmpstore AxlP5Vol -guid $VOLGUID"
def start2    "$START"
def regen     'sshd.efi stop' 'sshd.efi hostkey --regenerate'
def start3    "$START"
def dumpT     'sshd.efi stop' 'sshd.efi -T --host-key rsa_file --host-key hk_file'
def files1    "$START --host-key rsa_file --host-key hk_file"
def files2    'sshd.efi stop' "$START --host-key rsa_file"
def files3    "$START --host-key bad_seed --host-key hk_file"
def hkfiles   'sshd.efi stop' 'sshd.efi hostkey --host-key bad_seed --host-key hk_file'
def import1   'sshd.efi hostkey --import hk_file'
def import2   'sshd.efi hostkey --import hk_file --force'
def import3   'sshd.efi hostkey'
def denied    'echo x > fs0:\sshd-fault-hk-delete' 'sshd.efi hostkey --regenerate' \
              'del fs0:\sshd-fault-hk-delete' 'sshd.efi hostkey'
def restore1  'echo x > fs0:\sshd-fault-hk-store-once' \
              'sshd.efi hostkey --import hk2_file --force' 'sshd.efi hostkey'
def restore2  'echo x > fs0:\sshd-fault-hk-store' 'sshd.efi hostkey --import hk2_file --force' \
              'del fs0:\sshd-fault-hk-store' 'sshd.efi hostkey'
def leftover  'echo x > fs0:\sshd-fault-hk-readback' 'echo x > fs0:\sshd-fault-hk-keep' \
              'sshd.efi hostkey --import hk2_file' 'del fs0:\sshd-fault-hk-readback' \
              'del fs0:\sshd-fault-hk-keep' "dmpstore AxlSshHostKeyEd25519 -guid $GUID"
def size      'sshd.efi hostkey --regenerate' \
              "setvar AxlSshHostKeyEd25519 -guid $GUID -nv -bs =0102" "$START" \
              "dmpstore AxlSshHostKeyEd25519 -guid $GUID"
def rt        'sshd.efi hostkey --regenerate' \
              "setvar AxlSshHostKeyEd25519 -guid $GUID -nv -bs -rt =01" "$START" \
              "dmpstore AxlSshHostKeyEd25519 -guid $GUID"
def mismatch  'sshd.efi hostkey --regenerate' \
              "setvar AxlSshHostKeyEd25519 -guid $GUID -nv -bs =$BAD" "$START" \
              "dmpstore AxlSshHostKeyEd25519 -guid $GUID"
def readerr   'echo x > fs0:\sshd-fault-hk-read' "$START" 'del fs0:\sshd-fault-hk-read' \
              "dmpstore AxlSshHostKeyEd25519 -guid $GUID"
def readdxe   'sshd.efi hostkey --regenerate' "$START" 'sshd.efi stop' \
              'echo x > fs0:\sshd-fault-hk-read-dxe' "$START" 'del fs0:\sshd-fault-hk-read-dxe' \
              "dmpstore AxlSshHostKeyEd25519 -guid $GUID" 'sshd.efi hostkey'
def degraded  'sshd.efi hostkey --regenerate' 'echo x > fs0:\sshd-fault-hk-store' "$START" \
              'sshd.efi status'
def degraded2 'sshd.efi stop' 'del fs0:\sshd-fault-hk-store' 'sshd.efi hostkey'

echo "=== sshd host key ($TEST_ARCH) ==="
SSHD_ALLOW_REBOOT=1
sshd_boot ""

# step LABEL -- run the staged script and wait for its end marker.
step() {
    STEP_TAG="$1"
    send "$1.nsh\r"
    wait_guest "HKT-$1-END" 1 90 || true
    test_clean_log
    awk -v b="HKT-$1-BEGIN" -v e="HKT-$1-END" '
        $0 == b { on = 1; next } on && $0 == e { exit } on { print }' \
        "$TEST_CLEAN_LOG" | tr -d '\r' > "$TEST_TMPDIR/step-$1.log"
    STEP_LOG="$TEST_TMPDIR/step-$1.log"
}
rc_is()    { [[ "$(grep -E "^HKT-$STEP_TAG-RC=" "$STEP_LOG" | tail -1)" == "HKT-$STEP_TAG-RC=$1" ]]; }
has()      { grep -qxF -- "$1" "$STEP_LOG"; }
has_re()   { grep -qxE -- "$1" "$STEP_LOG"; }
lacks_re() { ! grep -qE -- "$1" "$STEP_LOG"; }
show_step() { sed 's/^/      /' "$STEP_LOG" | tail -14; }
# The driver's log lines, as "LEVEL text": the timestamp, and whatever the
# console put in front of it, dropped -- the rest is compared whole.
drv_lines() {
    sed -nE 's/^.*[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+ \[([A-Z]+)\] +/\1 /p' "$STEP_LOG"
}
drv_has()    { drv_lines | grep -qxF -- "$1 $2"; }
drv_has_re() { drv_lines | grep -qxE -- "$1 $2"; }
# Every RC line of the step, in order, space-separated: "0x1 0x1".
rcs() { sed -n "s/^HKT-$STEP_TAG-RC=//p" "$STEP_LOG" | paste -sd' '; }
expect() {
    local label="$1"
    shift
    if "$@"; then pass "$label"; else fail "$label"; show_step; fi
}
# The fingerprint a start or status printed.
fp_of() { sed -n 's/^sshd: host key ED25519 key fingerprint is \(SHA256:[A-Za-z0-9+\/]*\)\.$/\1/p' "$STEP_LOG" | head -1; }
# A path as sshd prints one: the volume sshd.efi is on, then the file.
VOLRE='[A-Za-z0-9]+:\\'

# ssh(1) against the server, -v, no password (BatchMode): the host key is
# checked before authentication, so a refused login still shows it.
# $1 = transcript, $2 = StrictHostKeyChecking value, $3 = known_hosts file.
ssh_probe() {
    timeout 30 ssh -v -p "$HOST_PORT" -o BatchMode=yes -o ConnectTimeout=20 \
        -o StrictHostKeyChecking="$2" -o UserKnownHostsFile="$3" \
        -o PubkeyAuthentication=no -o PreferredAuthentications=password \
        probe@127.0.0.1 true > "$1" 2>&1 < /dev/null || true
    tr -d '\r' < "$1" > "$1.clean"
}
ssh_saw()   { grep -qxF "debug1: Server host key: ssh-ed25519 $2" "$1.clean"; }
ssh_known() { grep -qxF "debug1: Host '[127.0.0.1]:$HOST_PORT' is known and matches the ED25519 host key." "$1.clean"; }
ssh_refused_change() { grep -qF 'REMOTE HOST IDENTIFICATION HAS CHANGED' "$1.clean" \
                       && grep -qxF 'Host key verification failed.' "$1.clean"; }
UNUSABLE_FMT="sshd: the stored host key is unusable (%s); not starting. Run 'sshd hostkey --regenerate' to replace it"
unusable() { printf "$UNUSABLE_FMT" "$1"; }

# --- first ---------------------------------------------------------------------

step none
expect "first: nothing stored, and \`sshd hostkey\` says so (exit 0)" \
    eval 'has "sshd: no host key is stored yet; the first start generates one" && rc_is 0x0'

step start1
FP1=$(fp_of)
expect "first: the first start says it generated a key, and prints its fingerprint" \
    eval 'has "sshd: generated a new host key" && [[ -n "$FP1" ]] && rc_is 0x0'
expect "first: the driver's two lines, whole: generated, then the key it serves" \
    eval 'drv_has INFO "sshd: generated a new host key: ssh-ed25519 $FP1 (UEFI variable $VARNAME)" && drv_has INFO "sshd: host key ssh-ed25519 $FP1 (UEFI variable $VARNAME)"'

step status1
expect "first: status prints the same fingerprint" \
    has "sshd: host key ED25519 key fingerprint is $FP1."

step hk1
expect "first: \`sshd hostkey\` prints the running server's key" \
    eval 'has "sshd: the running server'"'"'s host key:" && has "ED25519 key fingerprint is $FP1." && has_re "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI[A-Za-z0-9+/]{43}"'

step running
STOP_FIRST="sshd: stop the server first ('sshd stop'); a running server keeps the host key it started with"
expect "running: --regenerate and --import are refused while the server runs (exit 1 each)" \
    eval '[[ "$(grep -cxF -- "$STOP_FIRST" "$STEP_LOG")" == 2 && "$(rcs)" == "0x1 0x1" ]]'

: > "$KH"
ssh_probe "$TEST_TMPDIR/ssh1.log" accept-new "$KH"
expect "first: ssh(1) is shown the fingerprint sshd printed" ssh_saw "$TEST_TMPDIR/ssh1.log" "$FP1"

# --- reboot ----------------------------------------------------------------------

step vol
expect "reboot CONTROL: a volatile variable is there before the reboot" \
    has "Variable BS '$VOLGUID_UC:AxlP5Vol' DataSize = 0x01"

send 'reset\r'
if wait_guest "SSHD-BOOTED" 2 180; then
    pass "reboot: the guest came back"
else
    fail "reboot: the guest came back"
fi
sleep 1
send 'set -v T p4f\r'

step vol2
expect "reboot CONTROL: the volatile variable is gone -- dmpstore says so, whole" \
    eval 'has "dmpstore: No matching variables found. Guid $VOLGUID_UC, Name AxlP5Vol" && lacks_re "'"'"'$VOLGUID_UC:AxlP5Vol'"'"'"'

step start2
expect "reboot: the start after it loads the SAME key, and does not generate one" \
    eval '[[ "$(fp_of)" == "$FP1" ]] && ! has "sshd: generated a new host key" && rc_is 0x0'
expect "reboot: the driver serves it from the variable, and generated nothing" \
    eval 'drv_has INFO "sshd: host key ssh-ed25519 $FP1 (UEFI variable $VARNAME)" && ! drv_lines | grep -q "generated a new host key"'

ssh_probe "$TEST_TMPDIR/ssh2.log" yes "$KH"
expect "reboot: ssh(1) with StrictHostKeyChecking=yes and its known_hosts lets it in" \
    ssh_known "$TEST_TMPDIR/ssh2.log"

# --- regen -------------------------------------------------------------------------

step regen
expect "regen: --regenerate deletes, then warns and names the OLD fingerprint" \
    eval 'has "sshd: deleted host key ED25519 $FP1" && has "sshd: the host key was deleted; the next start generates a new one and prints it" && has "sshd: WARNING: replacing the host key. Every client that has connected before" && rc_is 0x0'

step start3
FP2=$(fp_of)
expect "regen: the next start generates a DIFFERENT key" \
    eval 'has "sshd: generated a new host key" && [[ -n "$FP2" && "$FP2" != "$FP1" ]]'

ssh_probe "$TEST_TMPDIR/ssh3.log" yes "$KH"
expect "regen: ssh(1) refuses the changed key" ssh_refused_change "$TEST_TMPDIR/ssh3.log"

# --- files -------------------------------------------------------------------------

step dumpT
expect "files: -T prints one hostkey line per file, in order" \
    eval 'has_re "hostkey ${VOLRE}rsa_file" && has_re "hostkey ${VOLRE}hk_file" && [[ "$(grep -n "^hostkey " "$STEP_LOG" | head -1)" == *rsa_file ]]'

step files1
expect "files: the ssh-rsa file is refused by name, in sshd's two lines" \
    eval 'has_re "sshd: Unable to load host key \"${VOLRE}rsa_file\": ssh-rsa host keys are not supported; only ssh-ed25519" && has_re "sshd: Unable to load host key: ${VOLRE}rsa_file"'
expect "files: ...and by the driver, in the same two lines at ERROR" \
    eval 'drv_has_re ERROR "sshd: Unable to load host key \"${VOLRE}rsa_file\": ssh-rsa host keys are not supported; only ssh-ed25519" && drv_has_re ERROR "sshd: Unable to load host key: ${VOLRE}rsa_file"'
expect "files: the ed25519 file after it serves, with ssh-keygen -lf's fingerprint" \
    eval '[[ "$(fp_of)" == "$HK_FP" ]] && rc_is 0x0'
expect "files: the driver says it serves the file, by name" \
    drv_has_re INFO "sshd: host key ssh-ed25519 ${HK_FP_RE} \\(file ${VOLRE}hk_file\\)"
ssh_probe "$TEST_TMPDIR/ssh4.log" no /dev/null
expect "files: ssh(1) is shown ssh-keygen -lf's fingerprint" \
    ssh_saw "$TEST_TMPDIR/ssh4.log" "$HK_FP"

step files2
expect "files: with only a refused file, sshd's \"no hostkeys available\" (exit 1)" \
    eval 'has "sshd: no hostkeys available -- exiting." && rc_is 0x1'

step files3
expect "files: a seed that does not derive its public key passes the launcher (no refusal of its own)" \
    eval '! grep -qE "^sshd: Unable to load host key.*bad_seed" "$STEP_LOG"'
expect "files: a seed that does not derive its public key is refused by the driver, in sshd's two lines" \
    eval 'drv_has_re ERROR "sshd: Unable to load host key \"${VOLRE}bad_seed\": the private key does not match its public key" && drv_has_re ERROR "sshd: Unable to load host key: ${VOLRE}bad_seed"'
expect "files: ...and the good file after it serves (exit 0)" \
    eval '[[ "$(fp_of)" == "$HK_FP" ]] && rc_is 0x0 && drv_has_re INFO "sshd: host key ssh-ed25519 ${HK_FP_RE} \\(file ${VOLRE}hk_file\\)"'

step hkfiles
expect "files: \`sshd hostkey\` names the first READABLE file, and says the server checks it fully" \
    eval 'has_re "sshd: the first readable host key file, ${VOLRE}bad_seed \(the server checks it fully when it loads it\):" && rc_is 0x0'

# --- import ------------------------------------------------------------------------

step import1
expect "import: a stored key is not replaced without --force (exit 1)" \
    eval 'has "sshd: a host key is already stored; run '"'"'sshd hostkey --regenerate'"'"' first, or pass --force" && rc_is 0x1'

step import2
expect "import: --force stores the file's key, printing ssh-keygen -lf's fingerprint" \
    eval 'has_re "sshd: imported ${VOLRE}hk_file into UEFI variable AxlSshHostKeyEd25519" && has "ED25519 key fingerprint is $HK_FP." && rc_is 0x0'
expect "import: ...and says to confirm it at a start before removing the file" \
    has "sshd: start sshd and confirm this fingerprint before removing the key file; the server checks the key fully when it loads it"

step import3
expect "import: the stored key is now the file's" \
    eval 'has "sshd: the stored host key (UEFI variable AxlSshHostKeyEd25519):" && has "ED25519 key fingerprint is $HK_FP."'

# --- denied -----------------------------------------------------------------------

step denied
expect "denied: a refused delete names the real recovery, and the key stays" \
    eval 'has "sshd: the firmware refuses to delete the stored host key (an authenticated variable: only its signer can). Use a HostKey file instead, or clear NVRAM in firmware setup" && has "ED25519 key fingerprint is $HK_FP."'
expect "denied: ...and says nothing of a replacement it did not make" \
    eval '! grep -qE "^sshd: (WARNING|deleted host key)" "$STEP_LOG"'

# --- restore ----------------------------------------------------------------------

step restore1
expect "restore: a failed --force write puts the key it replaced back, and says so (exit 1)" \
    eval 'has "sshd: cannot store the host key (cannot write the variable); the previous host key, ED25519 $HK_FP, is stored again" && [[ "$(rcs)" == "0x1 0x0" ]]'
expect "restore: ...and it is that key that is stored" \
    eval 'has "sshd: the stored host key (UEFI variable AxlSshHostKeyEd25519):" && has "ED25519 key fingerprint is $HK_FP." && ! has "ED25519 key fingerprint is $HK2_FP."'

step restore2
expect "restore: when the old key cannot be put back either, it says NO key is stored" \
    eval 'has "sshd: cannot store the host key (cannot write the variable), and the previous one could not be put back: no host key is stored now; the next start generates a new one" && has "sshd: no host key is stored yet; the first start generates one"'

step leftover
expect "restore: a failed write that stays in the variable is named, not called \"nothing\" (exit 1)" \
    eval 'has "sshd: cannot store the host key (the variable did not read back); the variable holds a write that failed: run '"'"'sshd hostkey --regenerate'"'"'" && rc_is 0x1 && has "Variable NV+BS '"'"'$GUID_UC:AxlSshHostKeyEd25519'"'"' DataSize = 0x54"'

# --- corrupt -----------------------------------------------------------------------

step size
expect "corrupt: the wrong size is refused by name (exit 1), and the variable is left as it was" \
    eval 'has "$(unusable "the stored key has the wrong size")" && rc_is 0x1 && has "Variable NV+BS '"'"'$GUID_UC:AxlSshHostKeyEd25519'"'"' DataSize = 0x02"'

step rt
expect "corrupt: a variable the OS could read (runtime access) is refused by name (exit 1), and left as it was" \
    eval 'has "$(unusable "the stored key is readable by the OS (runtime access)")" && rc_is 0x1 && has "Variable NV+RT+BS '"'"'$GUID_UC:AxlSshHostKeyEd25519'"'"' DataSize = 0x01"'

step mismatch
expect "corrupt: a seed that does not derive its public key passes the launcher and is refused by the driver" \
    eval 'drv_has ERROR "sshd: the stored host key is unusable (the private key does not match its public key); not serving -- run '"'"'sshd hostkey --regenerate'"'"'" && has "sshd: could not start the server" && has "Variable NV+BS '"'"'$GUID_UC:AxlSshHostKeyEd25519'"'"' DataSize = 0x54"'

# --- readerr ----------------------------------------------------------------------

step readerr
expect "readerr: a read error is refused by name, never taken as absent, and nothing is written" \
    eval 'has "sshd: the stored host key cannot be read (cannot read the variable); not starting. Run '"'"'sshd hostkey --regenerate'"'"' to replace it" && has "Variable NV+BS '"'"'$GUID_UC:AxlSshHostKeyEd25519'"'"' DataSize = 0x54"'

step readdxe
FP3=$(fp_of)
expect "readdxe: the driver's backstop refuses a read error the launcher did not see" \
    eval '[[ -n "$FP3" ]] && drv_has ERROR "sshd: the stored host key cannot be read (cannot read the variable); not serving -- run '"'"'sshd hostkey --regenerate'"'"'" && has "sshd: could not start the server"'
expect "readdxe: ...and generates nothing: the stored key is the one the first start made" \
    eval '[[ "$(drv_lines | grep -cF "generated a new host key: ssh-ed25519")" == 1 ]] && has "Variable NV+BS '"'"'$GUID_UC:AxlSshHostKeyEd25519'"'"' DataSize = 0x54" && has "ED25519 key fingerprint is $FP3."'

# --- degraded ---------------------------------------------------------------------

step degraded
expect "degraded: a key that cannot be stored serves anyway, and says so" \
    has "sshd: WARNING: the host key could not be saved (cannot write the variable); it changes at every start"
expect "degraded: ...and the driver's ERROR line, whole" \
    drv_has ERROR "sshd: cannot save the host key in UEFI variable $VARNAME (cannot write the variable): serving with a key that changes at every start"
expect "degraded: status reports the service DEGRADED, with the reason" \
    has "sshd: running on $GUEST_IP (degraded: host key not saved; it changes at every start)"

step degraded2
expect "degraded: nothing was stored" \
    has "sshd: no host key is stored yet; the first start generates one"

sshd_poweroff
echo ""
echo "sshd host key ($TEST_ARCH): $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
