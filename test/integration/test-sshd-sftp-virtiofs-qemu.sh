#!/bin/bash
# test-meta: arch=x64 needs=ssh,ssh-keygen,sftp,virtiofsd est=20 local-only=0 builds=ARCH=x64,sshd,sftp-server guest=none
# guest=none: --mount: a host directory as a virtio-fs volume of its own,
#   fixed when QEMU starts.
# test-sshd-sftp-virtiofs-qemu.sh -- SFTP rename on a VIRTIOFS volume.
#
# test-sshd-sftp-qemu.sh renames only on the boot FAT (fs0) and a RAM disk.
# A rename on a volume served by OVMF's VirtioFsDxe failed with AXL_ERR
# until the backend sized the rename's EFI_FILE_INFO exactly: VirtioFsDxe
# (SimpleFsSetInfo.c, ValidateInfoStructure) requires the LAST CHAR16 of
# the buffer to be the name's NUL, and sizeof(EFI_FILE_INFO) left 8 bytes
# past it. FAT does not check, which is why nothing caught it.
#
# The guest boots with sshd.efi and SftpServer.efi on fs0 and a host
# directory mounted as FS1 (run-qemu.sh --mount); the startup script copies
# sshd.conf and authorized_keys from FS1 to beside sshd.efi, starts sshd and
# keeps the guest up. The host's sftp(1), in batch mode, then:
#   put    a line to /fs1/sftpv/up.txt;
#   rename it to /fs1/sftpv/renamed.txt (sftp(1) sends posix-rename);
#   rename it onto an EXISTING /fs1/sftpv/old.txt (a replace);
#   get    it back, and ls the directory.
# Asserted: sftp(1) exits 0 with its exact transcript; the host directory
# holds exactly old.txt with the uploaded bytes; the bytes come back.
#
# Usage: test/integration/test-sshd-sftp-virtiofs-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "$2"; FAIL=$((FAIL + 1)); }

for t in ssh ssh-keygen sftp python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t required" >&2; exit 2; }
done

WORK="$(mktemp -d)"
QEMU_PID=""
VFSD_PID=""
cleanup() {
    [[ -n "$QEMU_PID" ]] && kill -9 "$QEMU_PID" 2>/dev/null || true
    [[ -n "$VFSD_PID" ]] && kill "$VFSD_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 sshd sftp-server 2>&1 | tail -1
SSHD="$PREFIX/tools/sshd.efi"
SFTPD="$PREFIX/tools/SftpServer.efi"
[[ -f "$SSHD" && -f "$SFTPD" ]] || { echo "FAIL: sshd.efi / SftpServer.efi not built"; exit 1; }

HOST_PORT=$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')

# The mounted directory: sshd's two files, and the tree under test.
V="$WORK/vfs"
mkdir -p "$V/sftpv"
ssh-keygen -q -t ed25519 -N '' -C 'axl-sshd-vfs-test' -f "$WORK/key" </dev/null
cp "$WORK/key.pub" "$V/authorized_keys"
cp "$PROJECT_DIR/tools/sshd.conf" "$V/sshd.conf"
printf 'old\n' > "$V/sftpv/old.txt"
DL="$WORK/dl"
mkdir -p "$DL"
printf 'uploaded over sftp to virtiofs\n' > "$DL/up.txt"

cat > "$WORK/startup.nsh" <<'EOF'
fs0:
cd \
connect -r
stall 1000000
ifconfig -s eth0 dhcp
stall 3000000
cp -q FS1:\sshd.conf fs0:\sshd.conf
cp -q FS1:\authorized_keys fs0:\authorized_keys
sshd --log-port 0
stall 120000000
reset -s
EOF

LOG="$WORK/serial.log"
"$RUN_QEMU" --background --timeout 180 --net --hostfwd "$HOST_PORT:22" \
    --mount "$V" --extra "$SFTPD" --nsh "$WORK/startup.nsh" \
    --serial-log "$LOG" "$SSHD" > "$WORK/run.out" 2>&1
QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$WORK/run.out" || true)
VFSD_PID=$(grep -oP '^VIRTIOFSD_PID=\K\d+' "$WORK/run.out" || true)
[[ -n "$QEMU_PID" ]] || { echo "ERROR: no QEMU pid"; cat "$WORK/run.out"; exit 1; }

for _ in $(seq 1 900); do
    grep -aq 'sshd: serving the UEFI Shell' "$LOG" 2>/dev/null && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.1
done
grep -aq 'sshd: serving the UEFI Shell' "$LOG" 2>/dev/null \
    || { echo "ERROR: sshd never served"; tr -d '\r' < "$LOG" | tail -30; exit 1; }

cat > "$WORK/b.batch" <<'EOF'
put up.txt /fs1/sftpv/up.txt
rename /fs1/sftpv/up.txt /fs1/sftpv/renamed.txt
rename /fs1/sftpv/renamed.txt /fs1/sftpv/old.txt
get /fs1/sftpv/old.txt back.txt
ls -1 /fs1/sftpv
EOF
RC=0
(cd "$DL" && timeout 120 sftp -b "$WORK/b.batch" -P "$HOST_PORT" \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
    -o IdentityAgent=none -o LogLevel=ERROR -i "$WORK/key" anyone@127.0.0.1) \
    > "$WORK/b.out" 2> "$WORK/b.err" < /dev/null || RC=$?

want=$'sftp> put up.txt /fs1/sftpv/up.txt
sftp> rename /fs1/sftpv/up.txt /fs1/sftpv/renamed.txt
sftp> rename /fs1/sftpv/renamed.txt /fs1/sftpv/old.txt
sftp> get /fs1/sftpv/old.txt back.txt
sftp> ls -1 /fs1/sftpv
/fs1/sftpv/old.txt'
got="$(tr -d '\r' < "$WORK/b.out")"
if [[ "$RC" -eq 0 && "$got" == "$want" ]]; then
    pass "sftp(1) puts, renames twice (once onto an existing file) on virtiofs: exact transcript, exit 0"
else
    fail "sftp(1) failed on the virtiofs volume (exit $RC)" \
         "--- out:"$'\n'"$got"$'\n'"--- err:"$'\n'"$(cat "$WORK/b.err")"
fi
if [[ "$(cd "$V/sftpv" && find . | LC_ALL=C sort)" == $'.\n./old.txt' ]] \
   && cmp -s "$V/sftpv/old.txt" "$DL/up.txt"; then
    pass "the host directory holds exactly old.txt, with the uploaded bytes"
else
    fail "the host directory is not as the renames leave it" \
         "$(cd "$V/sftpv" && find . -type f -exec sh -c 'printf "%s: " "$1"; head -c 60 "$1"; echo' _ {} \;)"
fi
if cmp -s "$DL/back.txt" "$DL/up.txt"; then
    pass "the renamed file comes back byte for byte"
else
    fail "the renamed file did not come back"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
