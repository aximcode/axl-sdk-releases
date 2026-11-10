#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-axl-package-roots.sh -- `axl` on a machine with .deb/.rpm SDK roots
# beside install.sh's (AXL-Distribution-Design.md §17.5), and the project-pin
# hint.
#
# WHY. The packages returned in 5.0.0 as axl-sdk-<version> under /opt, with
# their own alternatives link (/opt/axl-sdk-package) -- two managers must not
# share install.sh's `current`. Each root says who placed it in its receipt
# (share/axl/.axl-receipt, §21). From that:
#   - `axl list` shows package roots too, labelled by owner, and marks each
#     manager's current;
#   - `axl use`, `update` and `uninstall` refuse when `axl` runs from a
#     package's root, naming the apt/dnf/alternatives command: two managers of
#     one tree is the failure (Google Cloud CLI's package installs disable its
#     own updater the same way);
#   - on a packages-only machine, `axl version` and `--print-prefix` name the
#     package's versioned root.
# And `axl use` with no version names a project's pin (.axl-sdk-version, in
# this directory or a parent) as a hint -- never switching to it.
#
# Synthetic trees only, the real `axl`; AXL_PACKAGE_ROOT stands in for /opt.
#
# Usage: ./test/integration/test-axl-package-roots.sh

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
set +e

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        test_host_pass "$1"
    else
        test_host_fail "$1"; echo "      got:  $2" | head -12; echo "      want: $3" | head -12
    fi
}

# root DIR VERSION [SOURCE] -- an SDK root with the real dispatcher in it, a
# stub installer that reports its arguments, and a receipt when SOURCE given.
root() {
    local d="$1" v="$2" src="${3:-}"
    mkdir -p "$d/bin" "$d/libexec/axl" "$d/share/axl"
    install -m 0755 "$PROJECT_DIR/scripts/axl" "$d/bin/axl"
    printf '#!/bin/sh\necho "axl-cc %s"\n' "$v" > "$d/bin/axl-cc"; chmod +x "$d/bin/axl-cc"
    printf 'echo "stub install.sh $*"\n' > "$d/libexec/axl/install.sh"
    echo "$v" > "$d/share/axl/version"
    [[ -z "$src" ]] || printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=%s\n' "$src" \
        > "$d/share/axl/.axl-receipt"
}
USR="$WORK/home/.local/share"
PKG="$WORK/opt"
root "$USR/axl-sdk-4.7.5" 4.7.5               # before receipts
root "$USR/axl-sdk-5.0.0" 5.0.0 install.sh
ln -sfn axl-sdk-5.0.0 "$USR/axl-sdk"
root "$PKG/axl-sdk-4.9.0" 4.9.0 deb
root "$PKG/axl-sdk-5.0.0" 5.0.0 deb
ln -sfn "$PKG/axl-sdk-5.0.0" "$PKG/axl-sdk-package"
export AXL_PACKAGE_ROOT="$PKG" HOME="$WORK/home"

# Sizes are du's, so they are dropped before comparing: name, marker, owner.
nosize() { sed -E 's/^(..[^ ]+) +[0-9.,]+[KMGT]? +/\1  /'; }

echo "=== axl list: both managers' roots, labelled ==="
got="$("$USR/axl-sdk-5.0.0/bin/axl" list | nosize)"
check "list names every root, its owner and each manager's current" "$got" \
"  axl-sdk-4.7.5  (no receipt)
* axl-sdk-5.0.0  install.sh
  axl-sdk-4.9.0  deb
+ axl-sdk-5.0.0  deb

* = current (install.sh).  + = current (the system packages).
'axl use <version>' switches install.sh roots; 'axl prune' removes old ones.
Package roots belong to apt/dnf: install or remove them there."

echo "=== the manager verbs refuse on a package root ==="
PAXL="$PKG/axl-sdk-5.0.0/bin/axl"
out="$("$PAXL" update 2>&1)"; rc=$?
check "update from a .deb root exits 1" "$rc" "1"
check "...and says what to run instead" "$out" \
"axl: this SDK (axl-sdk-5.0.0) is a deb package, which apt manages -- 'axl update' is for install.sh's roots.
     A newer one installs beside it: sudo apt install ./axl-sdk-<version>_<version>-1_amd64.deb"
out="$("$PAXL" use 4.9.0 2>&1)"; rc=$?
check "use from a .deb root exits 1" "$rc" "1"
check "...naming the alternatives switch" "$out" \
"axl: this SDK (axl-sdk-5.0.0) is a deb package, which apt manages -- 'axl use' is for install.sh's roots.
     Switch the packages' current with: sudo update-alternatives --config axl-sdk-package"
out="$("$PAXL" uninstall 2>&1)"; rc=$?
check "uninstall from a .deb root exits 1" "$rc" "1"
check "...naming the remove command" "$out" \
"axl: this SDK (axl-sdk-5.0.0) is a deb package, which apt manages -- 'axl uninstall' is for install.sh's roots.
     Remove it with: sudo apt remove axl-sdk-5.0.0"
# prune too: run from a package root it would prune install.sh's versions
# and the toolchains on behalf of a user whose SDK apt manages.
mkdir -p "$PKG/axl-sdk-5.0.0/libexec/axl"
install -m 0755 "$PROJECT_DIR/scripts/axl-prune.sh" "$PKG/axl-sdk-5.0.0/libexec/axl/axl-prune.sh"
out="$("$PAXL" prune 2>&1)"; rc=$?
check "prune from a .deb root refuses" "$rc $out" \
"1 axl prune: this SDK (axl-sdk-5.0.0) is a deb package, which apt manages -- 'axl prune' is for install.sh's versions.
           Remove a package with: sudo apt remove axl-sdk-<version>"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=rpm\n' > "$PKG/axl-sdk-5.0.0/share/axl/.axl-receipt"
out="$("$PAXL" uninstall 2>&1)"; rc=$?
check "an .rpm root names dnf" "$rc $out" \
"1 axl: this SDK (axl-sdk-5.0.0) is an rpm package, which dnf manages -- 'axl uninstall' is for install.sh's roots.
     Remove it with: sudo dnf remove axl-sdk-5.0.0"
printf 'AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=deb\n' > "$PKG/axl-sdk-5.0.0/share/axl/.axl-receipt"
out="$("$USR/axl-sdk-5.0.0/bin/axl" use 4.7.5 2>&1)"; rc=$?
check "control: use from an install.sh root still reaches the installer" "$rc $out" \
    "0 stub install.sh --use 4.7.5 --prefix $USR --bin-dir $WORK/home/.local/bin"

echo "=== a packages-only machine ==="
# The package's link is all there is: no install.sh root, no `current`.
check "--print-prefix names the package's versioned root" \
    "$("$PKG/axl-sdk-package/bin/axl" --print-prefix)" "$PKG/axl-sdk-5.0.0"
check "version names it as both manager and SDK, and its owner" \
    "$("$PKG/axl-sdk-package/bin/axl" version | head -3)" \
"manager   5.0.0      $PKG/axl-sdk-5.0.0
sdk       5.0.0      $PKG/axl-sdk-5.0.0
owner     deb        managed by apt (axl-sdk-5.0.0)"

echo "=== axl use with no version: the project's pin, as a hint ==="
mkdir -p "$WORK/proj/sub/deeper" "$WORK/nopin" "$WORK/bad"
echo "4.7.5" > "$WORK/proj/.axl-sdk-version"
out="$(cd "$WORK/proj/sub/deeper" && "$USR/axl-sdk-5.0.0/bin/axl" use 2>&1)"; rc=$?
check "use with no version still exits 2" "$rc" "2"
check "...and names the pin found in a parent, with the command" "$out" \
"axl: use needs a version (try 'axl list')
axl: this project pins 4.7.5 ($WORK/proj/.axl-sdk-version) -- to switch to it: axl use 4.7.5"
out="$(cd "$WORK/nopin" && "$USR/axl-sdk-5.0.0/bin/axl" use 2>&1)"; rc=$?
check "no pin anywhere: the message is unchanged" "$rc $out" "2 axl: use needs a version (try 'axl list')"
printf '4.7.5; rm -rf /\n' > "$WORK/bad/.axl-sdk-version"
out="$(cd "$WORK/bad" && "$USR/axl-sdk-5.0.0/bin/axl" use 2>&1)"; rc=$?
check "a malformed pin: the message is unchanged" "$rc $out" "2 axl: use needs a version (try 'axl list')"
out="$(cd "$WORK/proj" && "$USR/axl-sdk-5.0.0/bin/axl" update --no-toolchain 2>&1)"
check "update names the pin it is about to leave" \
    "$(grep -F 'this project pins' <<<"$out")" \
    "axl: this project pins 4.7.5 ($WORK/proj/.axl-sdk-version); 'update' moves to the newest release -- to return to the pin: axl use 4.7.5"

echo "=== a receipt this user cannot read ==="
if [[ "$(id -u)" -ne 0 ]]; then
    chmod 000 "$USR/axl-sdk-5.0.0/share/axl/.axl-receipt"
    got="$("$USR/axl-sdk-5.0.0/bin/axl" list | nosize | grep 'axl-sdk-5.0.0  ' | head -1)"
    chmod 644 "$USR/axl-sdk-5.0.0/share/axl/.axl-receipt"
    check "list labels it unreadable, not '(no receipt)'" "$got" "* axl-sdk-5.0.0  unreadable"
else
    echo "  SKIP: an unreadable receipt (running as root, which reads anything)"
fi

test_host_summary "axl-package-roots"
