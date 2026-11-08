#!/bin/bash
# test-meta: arch=none needs= est=6 local-only=0
# test-toolchain-mirror.sh -- the aa64 toolchain download survives a proxy
# that refuses developer.arm.com, and says what came back when it fails.
#
# WHY THIS EXISTS. developer.arm.com only 302-redirects the tarball to ARM's
# blob store (armkeil.blob.core.windows.net); the bytes always come from there.
# In October 2026 www.arm.com moved to a certificate issued by the new
# "GlobalSign GCC E46 OV TLS CA 2025" intermediate, which Dell's intercepting
# proxy did not trust. The proxy then answered the developer.arm.com request
# with its own "Certificate Error" HTML page, so on a Dell laptop:
#   - over HTTP/2 curl died: "(56) Remote peer returned unexpected data while
#     we expected SETTINGS frame";
#   - over HTTP/1.1 it got a 200 with 7 KB of HTML and the SHA256 check failed.
# The blob host was allowed and served the exact pinned tarball. So the
# installer fetches from the blob host first and falls back to
# developer.arm.com, the pinned SHA256 vouching for whichever answered, and a
# web page in place of a tarball is reported as one -- with its title -- rather
# than as a bare checksum mismatch.
#
# Driven through a staged copy of install-toolchain.sh whose manifest pins a
# small fixture tarball, and a STUB curl that answers per host. No network.
#
# Usage: ./test/integration/test-toolchain-mirror.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

WORK="$(mktemp -d -t axl-mirror.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
check() { if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi; return 0; }

# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
TC_NAME="$(basename "$AXL_AA64_TOOLCHAIN_DIR")"

# A fixture toolchain: just a g++ that reports the pinned version, which is
# what install_aa64 checks after extracting.
mkdir -p "$WORK/fx/$TC_NAME/bin"
printf '#!/bin/sh\necho "aarch64-none-elf-g++ (fixture) %s"\n' \
    "${AXL_AA64_TOOLCHAIN_VERSION%%.rel*}.1" > "$WORK/fx/$TC_NAME/bin/aarch64-none-elf-g++"
chmod +x "$WORK/fx/$TC_NAME/bin/aarch64-none-elf-g++"
tar -cJf "$WORK/good.tar.xz" -C "$WORK/fx" "$TC_NAME"
FX_SHA="$(sha256sum "$WORK/good.tar.xz" | cut -d' ' -f1)"
cat > "$WORK/block.html" <<'HTML'
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <title>Certificate Error</title>
</head>
<body><p>There is an issue with the SSL certificate of the server.</p>
<svg><title>warning icon</title></svg></body>
</html>
HTML

# The installer as it ships, beside a manifest that pins the fixture.
mkdir -p "$WORK/sdk"
cp "$PROJECT_DIR/scripts/install-toolchain.sh" "$PROJECT_DIR/scripts/axl-common.sh" "$WORK/sdk/"
sed "s/^AXL_AA64_TOOLCHAIN_SHA256=.*/AXL_AA64_TOOLCHAIN_SHA256=$FX_SHA/" \
    "$PROJECT_DIR/scripts/axl-toolchains.conf" > "$WORK/sdk/axl-toolchains.conf"

# Per-host answers: good (the fixture), html (the block page, 200), empty (a
# 200 with no body: like an old curl, the stub creates no output file) or fail.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/curl" <<STUB
#!/bin/bash
for a in "\$@"; do [[ "\$a" == --version ]] && { echo "curl 8.5.0 (stub)"; exit 0; }; done
out=""; url=""
while [[ \$# -gt 0 ]]; do
    case "\$1" in
        -o) out="\$2"; shift 2 ;;
        http*) url="\$1"; shift ;;
        *) shift ;;
    esac
done
printf '%s\n' "\$url" >> "$WORK/curl.urls"
printf '%s\n' "\$(dirname "\$out")" >> "$WORK/curl.dirs"
case "\$url" in
    https://armkeil.blob.core.windows.net/*) how="\${STUB_BLOB:-fail}" ;;
    https://developer.arm.com/*)             how="\${STUB_ARM:-fail}" ;;
    *)                                       how=fail ;;
esac
case "\$how" in
    good) cp "$WORK/good.tar.xz" "\$out" ;;
    html) cp "$WORK/block.html" "\$out" ;;
    empty) ;;
    *)    echo "curl: (56) stub failure for \$url" >&2; exit 56 ;;
esac
STUB
chmod +x "$WORK/bin/curl"

run_tc() {  # run_tc <name> <env...>
    local name="$1"; shift
    local keep="${KEEP_PREFIX:-0}"
    [[ "$keep" -eq 1 ]] || rm -rf "$WORK/p-$name"
    rm -rf "$WORK/curl.urls" "$WORK/curl.dirs" "$WORK/tmp-$name"
    mkdir -p "$WORK/tmp-$name"
    env PATH="$WORK/bin:$PATH" TMPDIR="$WORK/tmp-$name" "$@" timeout 60 \
        "$WORK/sdk/install-toolchain.sh" aa64 --prefix "$WORK/p-$name" > "$WORK/$name.log" 2>&1
}
# Download directories the run left in its private TMPDIR.
leftovers() { find "$WORK/tmp-$1" -mindepth 1 -maxdepth 1 -name 'axl-toolchain-*' | wc -l; }
nfetch() { if [[ -f "$WORK/curl.urls" ]]; then wc -l < "$WORK/curl.urls"; else echo 0; fi; }
receipt_src() { sed -n 's/^AXL_RECEIPT_SOURCE=//p' "$WORK/p-$1/$TC_NAME/.axl-receipt" 2>/dev/null; }
GXX_REL="$TC_NAME/bin/aarch64-none-elf-g++"

echo "=== aa64 toolchain mirror fallback ==="
echo ""

# 1. The blob host answers: one fetch, from it, and nothing else is tried.
run_tc blob STUB_BLOB=good STUB_ARM=fail; rc=$?
[[ "$rc" -eq 0 && -x "$WORK/p-blob/$GXX_REL" ]] && r=0 || r=1
check "$r" "blob host serves the tarball: installs (rc=$rc)"
[[ "$(nfetch)" -eq 1 ]] && grep -q '^https://armkeil.blob.core.windows.net/' "$WORK/curl.urls"
check $? "...with one fetch, from the blob host ($(nfetch) fetch(es))"
[[ "$(receipt_src blob)" == https://armkeil.blob.core.windows.net/* ]] && r=0 || r=1
check "$r" "...and the receipt records the URL that answered: $(receipt_src blob)"
# The download really went into the private TMPDIR -- or "nothing left" there
# would prove nothing -- and that directory is gone afterwards.
[[ "$(leftovers blob)" -eq 0 ]] && grep -q "^$WORK/tmp-blob/axl-toolchain-" "$WORK/curl.dirs" \
    && r=0 || r=1
check "$r" "...and the downloaded tarball is cleaned up ($(leftovers blob) dir(s) left)"

# 2. The blob host sends a proxy page: it is named, then arm.com serves.
run_tc blob-html STUB_BLOB=html STUB_ARM=good; rc=$?
[[ "$rc" -eq 0 && -x "$WORK/p-blob-html/$GXX_REL" ]] && r=0 || r=1
check "$r" "blob host returns a web page: falls back to developer.arm.com and installs (rc=$rc)"
grep -qF '("Certificate Error",' "$WORK/blob-html.log"
check $? "...and says the blob host sent a web page, with its FIRST title"
[[ "$(receipt_src blob-html)" == https://developer.arm.com/* ]] && r=0 || r=1
check "$r" "...and the receipt records developer.arm.com: $(receipt_src blob-html)"
# Re-running over the installed tree re-marks it, and must keep the source
# that actually delivered rather than claim the first URL in the list.
# Drop a line first: its return proves the receipt was rewritten, not just left.
sed -i '/^AXL_RECEIPT_ARCH=/d' "$WORK/p-blob-html/$TC_NAME/.axl-receipt"
KEEP_PREFIX=1 run_tc blob-html STUB_BLOB=good STUB_ARM=good; rc=$?
[[ "$rc" -eq 0 && "$(nfetch)" -eq 0 && "$(receipt_src blob-html)" == https://developer.arm.com/* ]] \
    && grep -q '^AXL_RECEIPT_ARCH=aa64$' "$WORK/p-blob-html/$TC_NAME/.axl-receipt" && r=0 || r=1
check "$r" "re-run over it: fetches nothing and keeps the receipt's source ($(receipt_src blob-html))"

# 3. The blob host fails outright: arm.com serves.
run_tc blob-fail STUB_BLOB=fail STUB_ARM=good; rc=$?
[[ "$rc" -eq 0 && -x "$WORK/p-blob-fail/$GXX_REL" ]] && r=0 || r=1
check "$r" "blob host unreachable: falls back to developer.arm.com and installs (rc=$rc)"

# 4. Mark's case with the mirror also gone: the proxy page from arm.com is
# reported as a web page with its title, not as a bare checksum mismatch.
run_tc both-bad STUB_BLOB=fail STUB_ARM=html; rc=$?
[[ "$rc" -ne 0 && ! -e "$WORK/p-both-bad/$GXX_REL" ]] && r=0 || r=1
check "$r" "both sources bad: fails and installs nothing (rc=$rc)"
grep -q 'developer.arm.com' "$WORK/both-bad.log" && grep -q 'web page' "$WORK/both-bad.log" \
    && grep -qF 'Certificate Error' "$WORK/both-bad.log"
check $? "...naming the web page developer.arm.com returned, and its title"
grep -qi 'proxy' "$WORK/both-bad.log"
check $? "...and pointing at the proxy as the likely cause"
[[ "$(nfetch)" -eq 2 ]] && r=0 || r=1
check "$r" "...after trying both sources ($(nfetch) fetch(es))"
[[ "$(leftovers both-bad)" -eq 0 ]] && r=0 || r=1
check "$r" "...and leaves no download behind ($(leftovers both-bad) dir(s) left)"
grep -qF 'AXL_INSECURE_FETCH=1 ' "$WORK/both-bad.log"
check $? "...and, with TLS checked, offers AXL_INSECURE_FETCH=1 as a way out"

# 5. The same with TLS verification already off: the hint must not offer the
# flag the user is already running with.
run_tc both-bad-k AXL_INSECURE_FETCH=1 STUB_BLOB=fail STUB_ARM=html
grep -q 'already OFF' "$WORK/both-bad-k.log" && ! grep -qF "AXL_INSECURE_FETCH=1 $WORK" "$WORK/both-bad-k.log"
check $? "AXL_INSECURE_FETCH=1 already set: the hint says so and does not offer it again"

# 6. A stale answer is not blamed on the next host: the blob host's page is
# gone before developer.arm.com's empty 200 is judged.
run_tc stale STUB_BLOB=html STUB_ARM=empty; rc=$?
[[ "$rc" -ne 0 ]] && grep -q 'developer.arm.com returned an empty response' "$WORK/stale.log" \
    && [[ "$(grep -c 'returned a web page' "$WORK/stale.log")" -eq 1 ]] && r=0 || r=1
check "$r" "an empty 200 is reported as empty, not as the previous host's web page (rc=$rc)"

test_host_summary "toolchain-mirror"
