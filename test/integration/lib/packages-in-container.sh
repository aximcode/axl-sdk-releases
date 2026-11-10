# shellcheck shell=sh
# packages-in-container.sh -- the in-container half of test-packages.sh.
#
# Run by `sh -c` in debian:stable-slim (FAM=deb) or fedora:latest (FAM=rpm),
# with scripts/container-pkg.sh prepended (pkg_install) and these mounted:
#   /pkgs/new.<ext>     the real axl-sdk-$V package make-release-assets.sh built
#   /pkgs/newer.<ext>   a fabricated axl-sdk-$FAKE_V, same maintainer scripts
#   /pkgs/old.<ext>     a fabricated 4.4.0-shaped `axl-sdk` owning /usr/bin/axl
#   /ex                 sdk/examples
# Prints `  PASS ...` / `  FAIL ...` lines, which the host counts.
# Sourced text, never executed: no shebang, mode 0644.

set -u
rc=0
ok()  { echo "  PASS $1"; }
bad() { echo "  FAIL $1"; rc=1; }
R="/opt/axl-sdk-$V"

# The dependencies and the test's own tools from the mirror; OUR packages are
# installed from files by the low-level tool, so a failure there is ours and
# says so, not a mirror's.
if [ "$FAM" = deb ]; then
    pkg_install curl xz-utils gcc libc6-dev binutils
    inst()   { dpkg -i "$1" > /tmp/inst.log 2>&1; }
    remove() { dpkg -r "$1" > /tmp/rm.log 2>&1; }
    owns()   { dpkg -s "$1" > /dev/null 2>&1; }
    files()  { dpkg -L "$1" 2>/dev/null; }
    ALT=update-alternatives
else
    # NOT alternatives: the package must pull it in (Requires(post)).
    pkg_install curl xz tar gcc binutils util-linux
    inst()   { rpm -U "$1" > /tmp/inst.log 2>&1; }
    remove() { rpm -e "$1" > /tmp/rm.log 2>&1; }
    owns()   { rpm -q "$1" > /dev/null 2>&1; }
    files()  { rpm -ql "$1" 2>/dev/null; }
    ALT=alternatives
    # The base image ships alternatives (and binutils needs it), so the
    # package's Requires(post) would go unproven. Take it away: rpm's own
    # dependency check must then refuse the package, NAMING it -- the direct
    # proof -- and the mirror (bounded, through pkg_install) puts it back.
    rpm -e --nodeps alternatives > /dev/null 2>&1
    rpm -q alternatives > /dev/null 2>&1 && bad "fixture: alternatives is still installed" \
        || ok "fixture: alternatives removed, so the package must require it"
    if rpm -U --test /pkgs/new.rpm > /tmp/req.log 2>&1; then
        bad "the package installs with no alternatives -- it does not require it"
    elif grep -q '/usr/sbin/alternatives is needed by' /tmp/req.log; then
        ok "rpm refuses it without alternatives, naming /usr/sbin/alternatives"
    else
        bad "rpm refused it, but not for alternatives"; cat /tmp/req.log
    fi
    pkg_install alternatives
fi

echo "--- 1. the 4.4.0-shaped axl-sdk, then the new package replaces it"
inst /pkgs/old.$FAM && [ -e /usr/bin/axl ] && ok "fixture: the old axl-sdk owns /usr/bin/axl" \
    || { bad "fixture: the old axl-sdk did not install"; cat /tmp/inst.log; }
if inst /pkgs/new.$FAM; then ok "axl-sdk-$V installs"; else bad "axl-sdk-$V did not install"; cat /tmp/inst.log; fi
owns axl-sdk && bad "the old axl-sdk is still installed" || ok "installing it removed the old axl-sdk"
[ -e /usr/bin/axl ] && bad "/usr/bin/axl is still there" || ok "and /usr/bin/axl went with it"

echo "--- 2. where it put things"
n_out="$(files "axl-sdk-$V" | grep -v "^$R" | grep -v '^/opt$' | grep -v '^/\.$' | head -5)"
[ -z "$n_out" ] && ok "every file it owns is under $R" || bad "files outside $R: $n_out"
[ "$(readlink -f /opt/axl-sdk-package)" = "$R" ] && ok "/opt/axl-sdk-package is $R" \
    || bad "/opt/axl-sdk-package -> $(readlink -f /opt/axl-sdk-package 2>&1)"
[ -e /opt/axl-sdk ] && bad "it created install.sh's /opt/axl-sdk" || ok "it left install.sh's /opt/axl-sdk alone"
[ "$(sed -n 's/^AXL_RECEIPT_SOURCE=//p' "$R/share/axl/.axl-receipt" 2>/dev/null)" = "$FAM" ] \
    && ok "its receipt says $FAM" || bad "receipt: $(cat "$R/share/axl/.axl-receipt" 2>&1)"

echo "--- 3. PATH: the profile.d script, and a per-user install wins"
useradd -m -s /bin/bash tester
got="$(su - tester -c 'command -v axl-cc' 2>/dev/null)"
[ "$got" = "/opt/axl-sdk-package/bin/axl-cc" ] && ok "a login shell finds axl-cc through profile.d" \
    || bad "command -v axl-cc in a login shell: '$got'"
# APPENDED, not prepended: the package's directory comes after every system
# directory, so a user's own -- and anything a site puts on PATH before the
# profile scripts run -- precedes it. (The ~/.local/bin case below holds
# either way, because ~/.profile prepends it later; this is the assertion
# that tells the two apart.)
got="$(su - tester -c 'printf %s "$PATH" | tr : "\n" | tail -1' 2>/dev/null)"
[ "$got" = "/opt/axl-sdk-package/bin" ] && ok "profile.d appends: the package bin is the last PATH entry" \
    || bad "the last PATH entry in a login shell is '$got'"
su - tester -c 'mkdir -p ~/.local/bin && printf "#!/bin/sh\necho user\n" > ~/.local/bin/axl && chmod +x ~/.local/bin/axl'
got="$(su - tester -c 'command -v axl' 2>/dev/null)"
[ "$got" = "/home/tester/.local/bin/axl" ] && ok "a ~/.local/bin axl wins over the package's" \
    || bad "command -v axl with ~/.local/bin/axl present: '$got'"

echo "--- 4. the package's axl"
[ "$(/opt/axl-sdk-package/bin/axl --print-prefix)" = "$R" ] && ok "--print-prefix names $R" \
    || bad "--print-prefix: $(/opt/axl-sdk-package/bin/axl --print-prefix 2>&1)"
/opt/axl-sdk-package/bin/axl update > /tmp/upd.log 2>&1; urc=$?
[ "$urc" = 1 ] && grep -Eq "is an? $FAM package" /tmp/upd.log \
    && ok "axl update refuses on the package root" || { bad "axl update rc=$urc"; cat /tmp/upd.log; }
if AXL_TOOLCHAIN=auto /opt/axl-sdk-package/bin/axl-cc /ex/hello.c -o /tmp/hello.efi > /tmp/cc.log 2>&1 \
   && [ "$(od -An -c -N2 /tmp/hello.efi | tr -d ' ')" = "MZ" ]; then
    ok "axl-cc builds hello.c with the host gcc"
else
    bad "axl-cc hello.c"; tail -5 /tmp/cc.log
fi

echo "--- 5. a newer package beside it, then removal"
if inst /pkgs/newer.$FAM; then ok "axl-sdk-$FAKE_V installs beside it"; else bad "axl-sdk-$FAKE_V"; cat /tmp/inst.log; fi
[ -d "$R" ] && ok "$R is still there" || bad "$R went away"
[ "$(readlink -f /opt/axl-sdk-package)" = "/opt/axl-sdk-$FAKE_V" ] \
    && ok "the newest is the packages' current" || bad "current -> $(readlink -f /opt/axl-sdk-package)"
remove "axl-sdk-$FAKE_V" && ok "removing it succeeds" || { bad "remove $FAKE_V"; cat /tmp/rm.log; }
[ "$(readlink -f /opt/axl-sdk-package)" = "$R" ] && ok "current moved back to $V" \
    || bad "after removing $FAKE_V, current -> $(readlink -f /opt/axl-sdk-package 2>&1)"
remove "axl-sdk-$V" && ok "removing axl-sdk-$V succeeds" || { bad "remove $V"; cat /tmp/rm.log; }
left="$(ls -d /opt/axl-sdk* /etc/profile.d/axl-sdk.sh 2>/dev/null)"
[ -z "$left" ] && ok "nothing is left in /opt or /etc/profile.d" || bad "left behind: $left"
$ALT --display axl-sdk-package > /dev/null 2>&1 && bad "the alternative is still registered" \
    || ok "and the alternative is gone"
exit $rc
