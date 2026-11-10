#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen est=60 local-only=0 builds=sshd guest=none
# guest=none: it types into the guest Shell (test_send_stdin), and a leased
#   guest takes no typed serial input.
# test-sshd-conf-qemu.sh -- sshd.conf: the settings file beside sshd.efi,
# read in OpenSSH sshd_config(5) syntax, with the command line's flags
# winning over it, `sshd -T` printing the result, and the running server
# doing what the file says.
#
# The host types into the Shell over the serial console (test_enable_stdin).
# Each `sshd` run below is bracketed by marker lines, and the lines between
# them (less the Shell's echo of what was typed) are compared EXACTLY: every
# line `sshd -T` prints, every refusal, and nothing else.
#
# Staged (the .conf files written by the HOST, CRLF, as a Windows editor
# leaves them): authorized_keys at the root; good.conf (a comment, mixed
# case, `Keyword=value`, a quoted path with a space, and `AxlLogPort 0`,
# which puts the live server's log on the console's own port, where the
# checks below read it -- by default sshd writes nothing to a port the
# console draws on); `my banner.txt`;
# bad1.conf (an unknown keyword on line 2); bad2.conf (MaxSessions out of
# range); warn.conf (a keyword OpenSSH reads and ignores, with a warning);
# big.conf / big.txt (a Banner one byte over the limit); nokeys.conf (an
# AuthorizedKeysFile that is not there).
#
# Scenarios, in run order:
#   t-defaults       no sshd.conf: `sshd -T` prints sshd's defaults, silently,
#                    with the default authorized_keys found at the root
#   t-shipped        the sshd.conf the tools tarball ships (tools/sshd.conf):
#                    `sshd -T -f` it prints exactly the defaults, and so does
#                    the same file with every keyword line uncommented -- so
#                    each value it documents IS the default (AxlPassword, a
#                    placeholder with no default text, stays commented). On
#                    the host: every keyword `sshd -T` prints is documented
#                    there once, and nothing else is
#   t-file           `sshd -T -f good.conf`: the file's values, a relative
#                    Banner placed beside sshd.efi
#   t-precedence     `-p 2223 -R` win over the file's Port and AxlReadOnly
#   t-implied-*      a given -P / -k implies PasswordAuthentication /
#                    PubkeyAuthentication yes over the file's no
#   t-flags-win      -P, -x, --read-only=false, -k, --net-timeout and
#                    --tcp-linger each win over the file
#   t-empty-pw/keys  an empty -P / -k is "none": it clears the file's
#                    AxlPassword / AuthorizedKeysFile and turns nothing on
#   t-warn           an ignored OpenSSH keyword is said, with its line, and
#                    the rest of the file still applies
#   refuse-unknown   `Bogus` on line 2 refuses to start, status 1
#   refuse-range     MaxSessions 11 refuses with the range
#   refuse-placeholder  the shipped sshd.conf with EVERY line uncommented,
#                    AxlPassword's placeholder included: the placeholder is
#                    not a password, so it is refused at its line
#   refuse-missing   a missing -f file is `cannot read`, not the defaults
#   refuse-banner    a Banner over 3968 bytes refuses at its line
#   refuse-utf8      a Banner file that is not UTF-8 refuses at its line (the
#                    SSH banner field is UTF-8; the server would refuse it)
#   refuse-keys      a missing AuthorizedKeysFile refuses at its line
#   refuse-exempt    a -x range the server would refuse is refused up front
#   ucs2-conf        a file the Shell wrote (`echo ... > u.conf`, UCS-2) reads
#   live             good.conf as fs0:\sshd.conf, plain `sshd`: it listens on
#                    the file's port; a key logs in; ssh(1) shows the Banner
#                    file's text once, then the console-size line; a password
#                    login -- with the file's own AxlPassword -- is refused
#                    (PasswordAuthentication no): ssh(1) is offered publickey
#                    only, and ssh-peer.py sending that password is refused
#   t-while-running  with the key session open: `sshd -T` still prints, a
#                    broken -f file is still refused at its line (it is read
#                    BEFORE the already-running check), plain `sshd` says it
#                    is already running, and the session still works
#   t-subdir         sshd.efi copied to fs0:\tools with its own sshd.conf and
#                    authorized_keys, run from fs0:\keys: `-T` shows the
#                    sshd.conf beside it and its authorized_keys, never the
#                    root's sshd.conf
#   live-limits      `sshd -f limits.conf`: MaxAuthTries 1 ends a connection at
#                    its first wrong password, and LoginGraceTime 2 drops one
#                    that never authenticates -- the driver applies them
#   live-flag-pw     sshd.conf says PasswordAuthentication no; `sshd -P` still
#                    serves the -P password (a real ssh(1) login), not the
#                    file's AxlPassword
#   live-shell-banner  a Banner the Shell wrote (`echo ... > fs0:\b.txt`,
#                    UTF-16LE + BOM) serves: ssh(1) shows its text, then the
#                    console-size line, before it asks for the password
#   refuse-dir       sshd.conf a DIRECTORY: `sshd` refuses with `cannot read`
#                    rather than running on its defaults
#
# Usage: ./test/integration/test-sshd-conf-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"

# The sessions are real ones.
sshd_require_tools ssh ssh-keygen

test_parse_args "$@"
test_setup

# The live server listens where good.conf says; the forward is set at boot.
GUEST_PORT=2222
PW=p4h-pw

sshd_build
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
printf '%s\r\n' '# test sshd.conf' 'port 2222' 'PasswordAuthentication=no' \
    "AxlPassword $PW" 'MaxAuthTries 2' 'LoginGraceTime 1m30s' \
    'Banner "my banner.txt"' 'PerSourcePenaltyExemptList 10.0.2.2/32' \
    'AxlLogPort 0' > "$TEST_STAGING/good.conf"
printf 'Authorized use only.\r\n' > "$TEST_STAGING/my banner.txt"
printf '# x\r\nBogus 1\r\n' > "$TEST_STAGING/bad1.conf"
printf 'MaxSessions 11\r\n' > "$TEST_STAGING/bad2.conf"
printf 'UseLogin no\r\nPort 2225\r\n' > "$TEST_STAGING/warn.conf"
printf 'Banner big.txt\r\n' > "$TEST_STAGING/big.conf"
head -c 3969 /dev/zero | tr '\0' 'b' > "$TEST_STAGING/big.txt"
printf 'AuthorizedKeysFile nokeys\r\n' > "$TEST_STAGING/nokeys.conf"
printf 'LogLevel VERBOSE\r\n' > "$TEST_STAGING/loglevel.conf"
printf 'LogLevel bogus\r\n' > "$TEST_STAGING/badlog.conf"
# Every setting a flag also reaches, set the other way in the file.
printf '%s\r\n' 'PasswordAuthentication no' 'PubkeyAuthentication no' \
    'AxlPassword file-pw' 'PerSourcePenaltyExemptList 10.0.2.2/32' \
    'AxlReadOnly yes' 'AuthorizedKeysFile authorized_keys' 'AxlNetworkTimeout 30' \
    'AxlTcpLinger 9000' > "$TEST_STAGING/prec.conf"
mkdir -p "$TEST_STAGING/keys"
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/keys/ak"
printf '# Latin-1, not UTF-8\r\nBanner latin1.txt\r\n' > "$TEST_STAGING/latin1.conf"
printf 'caf\xe9\r\n' > "$TEST_STAGING/latin1.txt"
printf '%s\r\n' 'Port 2222' 'MaxAuthTries 1' 'LoginGraceTime 2' "AxlPassword $PW" \
    'PerSourcePenaltyExemptList 10.0.2.2/32' 'AxlLogPort 0' > "$TEST_STAGING/limits.conf"
printf '%s\r\n' 'Port 2222' 'Banner b.txt' 'PerSourcePenaltyExemptList 10.0.2.2/32' \
    > "$TEST_STAGING/sbanner.conf"
# The sshd.conf the tools tarball ships, as shipped and with every keyword
# line uncommented. Two stay commented, being examples rather than defaults:
# AxlPassword's is a placeholder, and HostKey's names a key FILE where the
# default is the key stored in a UEFI variable, which no HostKey line writes.
SHIPPED_CONF="$PROJECT_DIR/tools/sshd.conf"
cp "$SHIPPED_CONF" "$TEST_STAGING/shipped.conf"
sed -E '/^#(AxlPassword|HostKey) /!s/^#([A-Z][A-Za-z]+ )/\1/' "$SHIPPED_CONF" \
    > "$TEST_STAGING/uncommented.conf"
sed -E 's/^#([A-Z][A-Za-z]+ )/\1/' "$SHIPPED_CONF" > "$TEST_STAGING/placeholder.conf"
PLACEHOLDER_LINE=$(grep -n '^#AxlPassword ' "$SHIPPED_CONF" | cut -d: -f1)
# A copy of sshd.efi in a directory, as a release tarball unpacks onto a
# stick, with its own sshd.conf and authorized_keys beside it.
mkdir -p "$TEST_STAGING/tools"
cp "$TEST_BUILD_DIR/tools/sshd.efi" "$TEST_STAGING/tools/sshd.efi"
printf 'Port 2226\r\n' > "$TEST_STAGING/tools/sshd.conf"
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/tools/authorized_keys"

# The launcher's line before it starts the driver.
STARTING='sshd: starting -- bringing up the network (up to 60 s)'

# sshd's defaults as `sshd -T` prints them, with the root authorized_keys.
defaults_block() {
    cat <<'EOF'
port 22
logingracetime 60
maxauthtries 3
maxsessions 10
clientaliveinterval 60
clientalivecountmax 3
unusedconnectiontimeout 300
pubkeyauthentication yes
passwordauthentication yes
banner none
persourcepenaltyexemptlist none
loglevel INFO
authorizedkeysfile FS0:\authorized_keys
maxstartups 10
rekeylimit 1073741824 0
persourcepenalties authfail:5 noauth:1 grace-exceeded:10 max:600 min:15
subsystem sftp SftpServer.efi
axlnic auto
axlnetworktimeout 60
axlpassword none
axlreadonly no
axlsftpreadonly no
axlreportgeometry no
axllogport auto
axltcplinger 0
hostkey uefi-variable:AxlSshHostKeyEd25519
EOF
}

# The same under a file that does not list Subsystem: a file serves only the
# subsystems it lists, as sshd_config does.
conf_block() {
    defaults_block | sed 's/^subsystem .*/subsystem none/'
}

# good.conf's settings over the defaults.
file_block() {
    conf_block | sed -e 's/^port 22$/port 2222/' \
        -e 's/^logingracetime 60$/logingracetime 90/' \
        -e 's/^maxauthtries 3$/maxauthtries 2/' \
        -e 's/^passwordauthentication yes$/passwordauthentication no/' \
        -e 's/^banner none$/banner FS0:\\my banner.txt/' \
        -e 's|^persourcepenaltyexemptlist none$|persourcepenaltyexemptlist 10.0.2.2/32|' \
        -e 's/^axlpassword none$/axlpassword set/' \
        -e 's/^axllogport auto$/axllogport 0/'
}

# run_case NAME COMMAND -- type COMMAND between markers, then its status.
# One step at a time: while sshd's take-over is in place, keys typed ahead
# of the Shell wait in the console's 63-key ring, and what overflows it is
# lost.
run_case() {
    send "echo %T%-b-$1\\r"
    wait_guest "p4f-b-$1" 1 30 || true
    send "$2\\r"
    send "echo %T%-rc-$1-%lasterror%\\r"
    wait_guest_re "^p4f-rc-$1-0x[0-9A-Fa-f]+\$" 1 60 || true
}

# What case $1 printed: the lines between its markers, less the Shell's echo
# of the typed command lines (they start at the prompt), the launcher's own
# clean leak verdict as it exits, the driver's serving line (checked on its
# own), and the driver's `sftp: off (<file> has no Subsystem line)` -- none
# of these test files lists Subsystem, and test-sshd-sftp-qemu.sh pins that
# line exactly -- and the host key's lines, the launcher's and the driver's
# (test-sshd-hostkey-qemu.sh grades each of them whole -- the launcher's, and
# the driver's after its timestamp; the key, and whether this
# start is the one that generated it, are not this file's subject). Any
# other log line stays, and fails the case.
case_out() {
    test_clean_log
    awk -v b="p4f-b-$1" -v e="p4f-rc-$1-" '
        $0 == b { on = 1; next }
        on && index($0, e) == 1 { exit }
        on && $0 ~ /^[0-9:.]+ \[INFO\]  mem: no leaks detected$/ { next }
        on && $0 ~ /[0-9:.]+ \[INFO\]  sshd: serving the UEFI Shell on 10\.0\.2\.15 port [0-9]+ \(console [0-9]+x[0-9]+\)$/ { next }
        on && $0 ~ /[0-9:.]+ \[INFO\]  sftp: off \([^ ]+\.conf has no Subsystem line\)$/ { next }
        on && $0 ~ /[0-9:.]+ \[INFO\]  sshd: (generated a new host key: |host key )ssh-ed25519 SHA256:[A-Za-z0-9+\/]+ \(UEFI variable AxlSshHostKeyEd25519\)$/ { next }
        on && $0 ~ /^sshd: generated a new host key$/ { next }
        on && $0 ~ /^sshd: host key ED25519 key fingerprint is SHA256:[A-Za-z0-9+\/]+\.$/ { next }
        on && $0 !~ /^FS0:\\[^>]*> / { print }' "$TEST_CLEAN_LOG"
}

# The status case $1 ended with (0x0, 0x1).
case_rc() {
    test_clean_log
    { grep -aoxE -- "p4f-rc-$1-0x[0-9A-Fa-f]+" "$TEST_CLEAN_LOG" || true; } | tail -1 \
        | sed -E "s/^p4f-rc-$1-//"
}

# check_case NAME LABEL EXPECTED-RC EXPECTED-OUTPUT
check_case() {
    local got rc
    got=$(case_out "$1")
    rc=$(case_rc "$1")
    if [[ "$got" == "$4" ]]; then
        pass "$2"
    else
        fail "$2"
        diff <(printf '%s\n' "$4") <(printf '%s\n' "$got") | sed 's/^/    /' || true
    fi
    check "$1: exits with status $3 ($rc)" test "$rc" = "$3"
}

echo "=== sshd.conf ($TEST_ARCH) ==="
sshd_boot ""

RAND=$(rand_tag)

# --- -T and the refusals, no server running -----------------------------------
run_case t-defaults 'sshd -T'
check_case t-defaults "t-defaults: no sshd.conf, \`sshd -T\` prints the defaults and nothing else" \
    0x0 "$(defaults_block)"

# The shipped sshd.conf: every keyword sshd -T prints is documented in it
# once -- commented out but for Subsystem, which ships on -- and it documents
# no other keyword.
shipped_keywords() {
    sed -nE 's/^#?([A-Z][A-Za-z]+) .*/\1/p' "$SHIPPED_CONF" | tr 'A-Z' 'a-z' | sort
}
check "t-shipped: the defaults list keywords to compare against ($(case_out t-defaults | wc -l))" \
    test "$(case_out t-defaults | wc -l)" -ge 19
check "t-shipped: tools/sshd.conf documents exactly the keywords sshd -T prints, once each" \
    test "$(shipped_keywords)" = "$(case_out t-defaults | cut -d' ' -f1 | sort)"
n=$(grep -cvE '^(#.*|Subsystem sftp SftpServer\.efi)?$' "$SHIPPED_CONF" || true)
check "t-shipped: every line of it is a comment or blank but the Subsystem line ($n others)" test "$n" = 0
n=$(grep -cx 'Subsystem sftp SftpServer.efi' "$SHIPPED_CONF" || true)
check "t-shipped: Subsystem sftp SftpServer.efi ships uncommented, once ($n)" test "$n" = 1
n=$(grep -c '^#AxlPassword ' "$SHIPPED_CONF" || true)
check "t-shipped: control: the uncommenting kept AxlPassword commented ($n)" \
    test "$n" = 1 -a "$(grep -c '^#' "$TEST_STAGING/uncommented.conf")" -gt 0 \
    -a "$(grep -c '^Port 22$' "$TEST_STAGING/uncommented.conf")" = 1
run_case t-shipped 'sshd -T -f shipped.conf'
check_case t-shipped "t-shipped: the shipped sshd.conf, as shipped, reads as the defaults" \
    0x0 "$(defaults_block)"
run_case t-uncommented 'sshd -T -f uncommented.conf'
check_case t-uncommented "t-shipped: uncommented, every value it documents is the default" \
    0x0 "$(defaults_block)"

run_case t-file 'sshd -T -f good.conf'
check_case t-file "t-file: \`sshd -T -f good.conf\` prints the file's settings" \
    0x0 "$(file_block)"

run_case t-precedence 'sshd -T -f good.conf -p 2223 -R'
check_case t-precedence "t-precedence: -p and -R win over the file" \
    0x0 "$(file_block | sed -e 's/^port 2222$/port 2223/' -e 's/^axlreadonly no$/axlreadonly yes/')"

# prec.conf's settings over the defaults.
prec_block() {
    conf_block | sed -e 's/^pubkeyauthentication yes$/pubkeyauthentication no/' \
        -e 's/^passwordauthentication yes$/passwordauthentication no/' \
        -e 's|^persourcepenaltyexemptlist none$|persourcepenaltyexemptlist 10.0.2.2/32|' \
        -e 's/^axlpassword none$/axlpassword set/' \
        -e 's/^axlreadonly no$/axlreadonly yes/' \
        -e 's/^axlnetworktimeout 60$/axlnetworktimeout 30/' \
        -e 's/^axltcplinger 0$/axltcplinger 9000/'
}

run_case t-implied-pw 'sshd -T -f prec.conf -P cli-pw'
check_case t-implied-pw "t-implied-pw: a given -P means PasswordAuthentication yes over the file's no" \
    0x0 "$(prec_block | sed 's/^passwordauthentication no$/passwordauthentication yes/')"

run_case t-implied-key 'sshd -T -f prec.conf -k keys\\ak'
check_case t-implied-key "t-implied-key: a given -k means PubkeyAuthentication yes over the file's no" \
    0x0 "$(prec_block | sed -e 's/^pubkeyauthentication no$/pubkeyauthentication yes/' \
                            -e 's/^authorizedkeysfile .*/authorizedkeysfile FS0:\\keys\\ak/')"

run_case t-flags-win 'sshd -T -f prec.conf -P cli-pw -x 192.0.2.0/24 --read-only=false -k keys\\ak --net-timeout 45 --tcp-linger 0'
check_case t-flags-win "t-flags-win: -P, -x, --read-only=false, -k, --net-timeout and --tcp-linger each win over the file" \
    0x0 "$(prec_block | sed -e 's/^pubkeyauthentication no$/pubkeyauthentication yes/' \
                            -e 's/^passwordauthentication no$/passwordauthentication yes/' \
                            -e 's|^persourcepenaltyexemptlist .*|persourcepenaltyexemptlist 192.0.2.0/24|' \
                            -e 's/^axlreadonly yes$/axlreadonly no/' \
                            -e 's/^axlnetworktimeout 30$/axlnetworktimeout 45/' \
                            -e 's/^axltcplinger 9000$/axltcplinger 0/' \
                            -e 's/^authorizedkeysfile .*/authorizedkeysfile FS0:\\keys\\ak/')"

# An EMPTY -P or -k is "none": it clears the file's AxlPassword or
# AuthorizedKeysFile and turns its method on no more than the file did.
run_case t-empty-pw 'sshd -T -f prec.conf -P ""'
check_case t-empty-pw "t-empty-pw: -P \"\" means no password -- the file's AxlPassword is cleared, PasswordAuthentication stays no" \
    0x0 "$(prec_block | sed 's/^axlpassword set$/axlpassword none/')"

run_case t-empty-keys 'sshd -T -f prec.conf -k ""'
check_case t-empty-keys "t-empty-keys: -k \"\" means no keys file -- the file's AuthorizedKeysFile is cleared, PubkeyAuthentication stays no" \
    0x0 "$(prec_block | sed 's/^authorizedkeysfile .*/authorizedkeysfile none/')"

# LogLevel: OpenSSH's names, -T in OpenSSH's place; -v and -d (OpenSSH
# sshd's -d) beat the file, as OpenSSH's -d does.
run_case t-loglevel 'sshd -T -f loglevel.conf'
check_case t-loglevel "t-loglevel: the file's LogLevel VERBOSE is printed in OpenSSH's place" \
    0x0 "$(conf_block | sed 's/^loglevel INFO$/loglevel VERBOSE/')"

run_case t-loglevel-v 'sshd -T -f loglevel.conf -v'
check_case t-loglevel-v "t-loglevel-v: -v wins over the file's LogLevel: DEBUG" \
    0x0 "$(conf_block | sed 's/^loglevel INFO$/loglevel DEBUG/')"

run_case t-loglevel-d 'sshd -T -f loglevel.conf -d'
check_case t-loglevel-d "t-loglevel-d: -d is -v" \
    0x0 "$(conf_block | sed 's/^loglevel INFO$/loglevel DEBUG/')"

run_case refuse-loglevel 'sshd -f badlog.conf'
check_case refuse-loglevel "refuse-loglevel: an unknown LogLevel refuses in OpenSSH's words" \
    0x1 "badlog.conf:1: unsupported log level 'bogus'"

run_case t-warn 'sshd -T -f warn.conf'
check_case t-warn "t-warn: an ignored OpenSSH keyword is said at its line, the rest applies" \
    0x0 "$(printf 'warn.conf:1: Deprecated option UseLogin\n'; conf_block | sed 's/^port 22$/port 2225/')"

run_case refuse-unknown 'sshd -f bad1.conf'
check_case refuse-unknown "refuse-unknown: an unknown keyword refuses to start, naming its line" \
    0x1 'bad1.conf:2: Bad configuration option: Bogus'

run_case refuse-range 'sshd -f bad2.conf'
check_case refuse-range "refuse-range: a value out of range refuses with the range" \
    0x1 'bad2.conf:1: MaxSessions: 11 is out of range (1-10)'

run_case refuse-placeholder 'sshd -T -f placeholder.conf'
check_case refuse-placeholder "refuse-placeholder: the shipped AxlPassword placeholder is refused at its line" \
    0x1 "placeholder.conf:$PLACEHOLDER_LINE: AxlPassword: replace the placeholder with a password"

run_case refuse-missing 'sshd -f nope.conf'
check_case refuse-missing "refuse-missing: a missing -f file is refused, not read as the defaults" \
    0x1 'sshd: cannot read FS0:\nope.conf'

run_case refuse-banner 'sshd -T -f big.conf'
check_case refuse-banner "refuse-banner: a Banner over the limit refuses at its line" \
    0x1 'big.conf:1: Banner: FS0:\big.txt is larger than 3968 bytes'

run_case refuse-utf8 'sshd -T -f latin1.conf'
check_case refuse-utf8 "refuse-utf8: a Banner that is not UTF-8 refuses at its line" \
    0x1 'latin1.conf:2: Banner: FS0:\latin1.txt is not UTF-8 text'

run_case refuse-keys 'sshd -T -f nokeys.conf'
check_case refuse-keys "refuse-keys: a missing AuthorizedKeysFile refuses at its line" \
    0x1 'nokeys.conf:1: AuthorizedKeysFile: cannot read FS0:\nokeys'

run_case refuse-exempt 'sshd -T -x 10.0.2.2/33'
check_case refuse-exempt "refuse-exempt: a bad -x range is refused by the launcher, by the server's rule" \
    0x1 'sshd: -x 10.0.2.2/33 is not an IPv4 address or CIDR range'

send 'echo Port 2224 > fs0:\\u.conf\r'
run_case ucs2-conf 'sshd -T -f u.conf'
check_case ucs2-conf "ucs2-conf: a file the Shell wrote in UCS-2 is read" \
    0x0 "$(conf_block | sed 's/^port 22$/port 2224/')"

check "nothing above deployed the server" test "$(guest_lines 'sshd: serving the UEFI Shell on 10.0.2.15 port 2222')" = 0

# --- live -----------------------------------------------------------------------
send 'cp good.conf sshd.conf\r'
run_case live 'sshd'
check_case live "live: plain \`sshd\` serves on sshd.conf's port" \
    0x0 "$(printf '%s\nsshd: settings from %s\nsshd: serving the UEFI Shell on 10.0.2.15 port 2222' "$STARTING" 'FS0:\sshd.conf')"
check "live: the driver logs its own serving line, on port 2222" sshd_read_geometry
ssh_open K
ssh_wait_repaint K 40 || true
sleep 1
ssh_type K "echo %T%-k$RAND\\r"
check "live: a key logs in, and the session runs a command" \
    wait_session "$(ssh_out K)" "p4f-k$RAND" 1 30
# GEOM is the size line sshd_read_geometry read from the driver's serving
# line; without it there is nothing to compare against, which must fail.
GEOM=${GEOM:-}
want=$(printf 'Authorized use only.\n%s' "$GEOM")
banner=$(tr -d '\r' < "$TEST_TMPDIR/K.err" | grep -axF -e 'Authorized use only.' -e "${GEOM:-Authorized use only.}" || true)
check "live: ssh(1) shows the Banner file's text once, then the console-size line" \
    test -n "$GEOM" -a "$banner" = "$want"

pw_err="$TEST_TMPDIR/pw.err"
pw_rc=0
timeout 60 ssh -p "$HOST_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes -o ConnectTimeout=20 -o PubkeyAuthentication=no \
    -o PreferredAuthentications=password -o IdentityAgent=none \
    anyone@127.0.0.1 true < /dev/null > /dev/null 2> "$pw_err" || pw_rc=$?
n=$(tr -d '\r' < "$pw_err" | grep -acxF 'anyone@127.0.0.1: Permission denied (publickey).' || true)
check "live: PasswordAuthentication no -- ssh(1) is offered publickey only, exit $pw_rc ($n)" \
    test "$pw_rc" = 255 -a "$n" = 1
peer_start pw "$PW" ""
wait "${PEER_PID[pw]}" 2>/dev/null || true
PEER_PID[pw]=""
check "live: the file's own AxlPassword, sent anyway, is refused" \
    peer_has pw "peer: auth failed"

# --- t-while-running --------------------------------------------------------------
run_case t-running 'sshd -T'
check_case t-running "t-while-running: \`sshd -T\` prints sshd.conf's settings" \
    0x0 "$(file_block | sed 's/^authorizedkeysfile .*/authorizedkeysfile FS0:\\authorized_keys/')"
run_case running-bad 'sshd -f bad1.conf'
check_case running-bad "t-while-running: a broken file is refused before the already-running check" \
    0x1 'bad1.conf:2: Bad configuration option: Bogus'
run_case running-again 'sshd'
check_case running-again "t-while-running: plain \`sshd\` says it is already running" \
    0x0 "sshd: already running - the running server was not reconfigured; run 'sshd stop' then 'sshd' again to change its options"
ssh_type K "echo %T%-k2$RAND\\r"
wait_session "$(ssh_out K)" "p4f-k2$RAND" 1 30 || true
sleep 1
n=$(session_count "$(ssh_out K)" "p4f-k2$RAND")
check "t-while-running: the key session still works, its line arriving once ($n)" test "$n" = 1
ssh_close K

# --- t-subdir ---------------------------------------------------------------------
# sshd.efi in fs0:\tools, run from another directory (fs0:\keys) while
# fs0:\sshd.conf (good.conf, Port 2222) is at the root: the settings and the
# default authorized_keys are the ones beside that sshd.efi.
send 'cd \\keys\r'
run_case t-subdir '\\tools\\sshd.efi -T'
send 'cd \\\r'
check_case t-subdir "t-subdir: sshd.efi in a directory reads the sshd.conf and authorized_keys beside it, not the root's" \
    0x0 "$(conf_block | sed -e 's/^port 22$/port 2226/' \
                            -e 's/^authorizedkeysfile .*/authorizedkeysfile FS0:\\tools\\authorized_keys/')"

# --- live-limits ---------------------------------------------------------------------
# Two server settings the driver must hand to AxlSsh, seen in its own audit
# lines: MaxAuthTries 1 ends a connection at its FIRST wrong password (the
# default, 3, would only log it), and LoginGraceTime 2 drops a connection
# that never authenticates.
send 'sshd stop\r'
wait_guest "sshd: stopped" 1 30 || true
run_case limits 'sshd -f limits.conf'
check_case limits "live-limits: \`sshd -f limits.conf\` serves" \
    0x0 "$(printf '%s\nsshd: settings from %s\nsshd: serving the UEFI Shell on 10.0.2.15 port 2222' "$STARTING" 'FS0:\limits.conf')"
peer_start wrong "not-$PW" ""
wait "${PEER_PID[wrong]}" 2>/dev/null || true
PEER_PID[wrong]=""
check "live-limits: MaxAuthTries 1 ends the connection at the first wrong password" \
    wait_guest_re "${LOG_PFX}ssh: maximum authentication attempts exceeded for anyone from 10\\.0\\.2\\.2 port [0-9]+\$" 1 20
exec {idle}<>"/dev/tcp/127.0.0.1/$HOST_PORT"
check "live-limits: LoginGraceTime 2 drops a connection that never authenticates" \
    wait_guest_re "${LOG_PFX}ssh: Timeout before authentication for connection from 10\\.0\\.2\\.2 port [0-9]+\$" 1 20
exec {idle}>&-
send 'sshd stop\r'
wait_guest "sshd: stopped" 2 30 || true

# --- live-flag-pw -----------------------------------------------------------------
# sshd.conf (good.conf) says PasswordAuthentication no and AxlPassword p4h-pw;
# `sshd -P cli-pw` must serve that password -- and only that one.
run_case flag-pw 'sshd -P cli-pw'
check_case flag-pw "live-flag-pw: \`sshd -P\` serves despite the file's PasswordAuthentication no" \
    0x0 "$(printf '%s\nsshd: settings from %s\nsshd: serving the UEFI Shell on 10.0.2.15 port 2222' "$STARTING" 'FS0:\sshd.conf')"
cat > "$TEST_TMPDIR/askpass.sh" <<'ASKPASS'
#!/bin/sh
printf '%s\n' cli-pw
ASKPASS
chmod +x "$TEST_TMPDIR/askpass.sh"
SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    timeout 60 ssh -v -p "$HOST_PORT" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o PubkeyAuthentication=no \
    -o KbdInteractiveAuthentication=no -o PreferredAuthentications=password \
    -o NumberOfPasswordPrompts=1 -o IdentityAgent=none \
    anyone@127.0.0.1 true > "$TEST_TMPDIR/flagpw.log" 2>&1 < /dev/null || true
n=$(tr -d '\r' < "$TEST_TMPDIR/flagpw.log" \
    | grep -acxE "Authenticated to 127\.0\.0\.1 \(\[127\.0\.0\.1\]:$HOST_PORT\) using \"password\"\." || true)
check "live-flag-pw: a real ssh(1) password login with the -P password succeeds ($n)" test "$n" = 1
peer_start filepw "$PW" ""
wait "${PEER_PID[filepw]}" 2>/dev/null || true
PEER_PID[filepw]=""
check "live-flag-pw: the file's AxlPassword is not accepted -- -P replaced it" \
    peer_has filepw "peer: auth failed"
send 'sshd stop\r'
wait_guest "sshd: stopped" 3 30 || true

# --- live-shell-banner -------------------------------------------------------------
# The Shell writes `echo` output as UTF-16LE with a BOM. The askpass program
# appends its marker to the file ssh(1)'s stderr appends to, so the order of
# the lines there is the order ssh(1) produced them in: the banner, then the
# password prompt.
send 'echo Shell-written banner > fs0:\\b.txt\r'
run_case shell-banner 'sshd -f sbanner.conf -P cli-pw'
check_case shell-banner "live-shell-banner: a Banner the Shell wrote is accepted, and sshd serves" \
    0x0 "$(printf '%s\nsshd: settings from %s\nsshd: serving the UEFI Shell on 10.0.2.15 port 2222' "$STARTING" 'FS0:\sbanner.conf')"
SB_LOG="$TEST_TMPDIR/sbanner.log"
: > "$SB_LOG"
cat > "$TEST_TMPDIR/askpass-sb.sh" <<ASKPASS
#!/bin/sh
printf 'ASKPASS-PROMPT\n' >> '$SB_LOG'
printf '%s\n' cli-pw
ASKPASS
chmod +x "$TEST_TMPDIR/askpass-sb.sh"
# `true` is an exec request, which sshd refuses after the login; -v says the
# login itself succeeded.
SSH_ASKPASS="$TEST_TMPDIR/askpass-sb.sh" SSH_ASKPASS_REQUIRE=force \
    timeout 60 ssh -v -p "$HOST_PORT" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o PubkeyAuthentication=no \
    -o KbdInteractiveAuthentication=no -o PreferredAuthentications=password \
    -o NumberOfPasswordPrompts=1 -o IdentityAgent=none \
    anyone@127.0.0.1 true > /dev/null 2>> "$SB_LOG" < /dev/null || true
SB_AUTH="Authenticated to 127.0.0.1 ([127.0.0.1]:$HOST_PORT) using \"password\"."
want=$(printf 'Shell-written banner\n%s\nASKPASS-PROMPT\n%s' "${GEOM:-no geometry line}" "$SB_AUTH")
got=$(tr -d '\r' < "$SB_LOG" | grep -axF -e 'Shell-written banner' -e "${GEOM:-no geometry line}" \
    -e 'ASKPASS-PROMPT' -e "$SB_AUTH" || true)
check "live-shell-banner: ssh(1) shows the Shell-written text, then the size line, then asks for the password, and logs in" \
    test -n "$GEOM" -a "$got" = "$want"
if [[ "$got" != "$want" ]]; then
    printf '%s\n' "$got" | sed 's/^/    got: /'
fi
send 'sshd stop\r'
wait_guest "sshd: stopped" 4 30 || true

# --- refuse-dir --------------------------------------------------------------------
send 'rm sshd.conf\r'
send 'mkdir sshd.conf\r'
run_case refuse-dir 'sshd'
check_case refuse-dir "refuse-dir: sshd.conf a directory refuses rather than running on defaults" \
    0x1 'sshd: cannot read FS0:\sshd.conf'

sshd_poweroff
n=$(heap_corruptions)
check "no heap corruption reported in the run ($n)" test "$n" = 0
test_clean_log
if test_check_leaks; then
    pass "leak gate: every verdict in the run is clean"
else
    fail "leak gate: a leak or a missing verdict"
fi

echo ""
printf "sshd-conf: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 85 ]] && exit 0 || exit 1
