#!/bin/bash
# test-meta: arch=none needs= est=6 local-only=0
# test-insecure-fetch.sh — AXL_INSECURE_FETCH, and what it does NOT buy.
#
# WHY THIS EXISTS. Dell MITMs HTTPS org-wide with a CA that a fresh WSL install
# does not trust, so `axl-install-toolchain` dies at TLS on a real coworker's
# machine even though the artifact it is about to fetch has a pinned SHA256
# shipped in the SDK. Requiring every such user to install a corporate CA is
# per-machine setup the hash makes unnecessary.
#
# THE TWO SITES ARE NOT EQUIVALENT, and that is the thing this test pins:
#
#   install-toolchain.sh  the expected hash comes from scripts/axl-toolchains.conf,
#                         which SHIPS IN THE SDK. Pre-shared, out-of-band, and an
#                         active attacker cannot forge an artifact to match it.
#                         `-k` here costs nothing.
#
#   install.sh            the expected hashes come from SHA256SUMS, fetched from
#                         the SAME base URL as the assets. Under `-k` a MITM
#                         serves both, so the guarantee drops from AUTHENTICATED
#                         to merely corruption-resistant -- unless the caller
#                         pinned hashes out-of-band, which the flagship consumer
#                         does in .axl-sdk-checksums.
#
# So install.sh must SAY so when the flag is on. Silence there would let the
# weaker of the two guarantees be read as the stronger one.
#
# Every fetch goes out as --http1.1 where curl accepts it. A proxy that answers
# with its own page in place of the file broke h2 framing on a Dell laptop
# ("(56) ... expected SETTINGS frame", developer.arm.com, 2026-11); HTTP/1.1
# delivers that page instead, which the installers can name (see
# test-toolchain-mirror.sh). And when the toolchain step fails anyway, the
# remedy install.sh prints must name an `axl` that will actually run -- not a
# bare name an old /usr/bin/axl shadows.
#
# Driven through a STUB curl that records its argv: no network, no real fetch.
#
# Usage: ./test/integration/test-insecure-fetch.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

WORK="$(mktemp -d -t axl-insec.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
check() { if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi; return 0; }

# A curl that logs its arguments and serves files from a local release dir, so
# the scripts run their real fetch path with no network.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/curl" <<STUB
#!/bin/bash
# --version is the installers' capability probe, not a fetch: answer, unlogged.
for a in "\$@"; do [[ "\$a" == --version ]] && { echo "curl 8.5.0 (stub)"; exit 0; }; done
printf '%s\n' "\$*" >> "$WORK/curl.argv"
out=""; url=""
while [[ \$# -gt 0 ]]; do
    case "\$1" in
        -o) out="\$2"; shift 2 ;;
        -O) out="basename"; shift ;;
        http*|file*) url="\$1"; shift ;;
        *) shift ;;
    esac
done
src="$WORK/rel/\$(basename "\$url")"
[[ -n "\$out" && "\$out" != basename ]] || out="\$(basename "\$url")"
[[ -f "\$src" ]] || exit 22
cp "\$src" "\$out"
STUB
chmod +x "$WORK/bin/curl"

echo "=== AXL_INSECURE_FETCH ==="
echo ""

# ---------------------------------------------------------------------------
# install.sh: the flag reaches curl, and only when asked.
# ---------------------------------------------------------------------------
REL="$WORK/rel"; mkdir -p "$REL"
V=9.9.9
mkdir -p "$WORK/t/axl-sdk-$V/bin" "$WORK/t/axl-sdk-$V/share/axl" "$WORK/t/axl-sdk-$V/libexec/axl"
printf '#!/bin/sh\necho axl\n' > "$WORK/t/axl-sdk-$V/bin/axl"; chmod +x "$WORK/t/axl-sdk-$V/bin/axl"
# A toolchain installer that always fails, for the remedy-hint checks below.
printf '#!/bin/sh\nexit 1\n' > "$WORK/t/axl-sdk-$V/bin/axl-install-toolchain"
chmod +x "$WORK/t/axl-sdk-$V/bin/axl-install-toolchain"
echo "$V" > "$WORK/t/axl-sdk-$V/share/axl/version"
install -m 0644 "$PROJECT_DIR/packaging/install.sh" "$WORK/t/axl-sdk-$V/libexec/axl/install.sh"
tar czf "$REL/axl-sdk-linux-$V-x86_64.tar.gz" -C "$WORK/t" "axl-sdk-$V"
echo "$V" > "$REL/VERSION"
( cd "$REL" && sha256sum -- *.tar.gz VERSION > SHA256SUMS )

run_install() {  # run_install <log> <env...>
    local log="$1"; shift
    rm -f "$WORK/curl.argv"
    # A FRESH prefix every time. already_installed short-circuits an unchanged
    # version and returns before fetching anything, so a reused prefix makes
    # every assertion about curl's arguments pass or fail for the wrong reason
    # -- it did, twice, while this test was being written.
    rm -rf "$WORK/p" "$WORK/b"
    env PATH="$WORK/bin:$PATH" "$@" HOME="$WORK/home" \
        sh "$PROJECT_DIR/packaging/install.sh" --yes --version "$V" \
        --base-url "file://$REL" --prefix "$WORK/p" --bin-dir "$WORK/b" \
        ${INSTALL_ARGS[@]+"${INSTALL_ARGS[@]}"} > "$log" 2>&1
}
INSTALL_ARGS=()

run_install "$WORK/off.log"
! grep -qE '(^| )-k( |$)' "$WORK/curl.argv" 2>/dev/null
check $? "default: no -k reaches curl"

run_install "$WORK/on.log" AXL_INSECURE_FETCH=1
grep -qE '(^| )-k( |$)' "$WORK/curl.argv" 2>/dev/null
check $? "AXL_INSECURE_FETCH=1: -k reaches curl"

# ...and it must SAY that its guarantee is weaker, because SHA256SUMS came
# down the same pipe as the asset it vouches for.
grep -qi 'same' "$WORK/on.log" && grep -qi 'SHA256SUMS' "$WORK/on.log"
check $? "install.sh warns that SHA256SUMS shares the channel"
! grep -qi 'AXL_INSECURE_FETCH' "$WORK/off.log"
check $? "...and says nothing about it when the flag is off"

# Every install.sh fetch goes out as HTTP/1.1, flag or not.
[[ -s "$WORK/curl.argv" ]] && ! grep -vqE '(^| )--http1\.1( |$)' "$WORK/curl.argv"
check $? "install.sh: every curl call carries --http1.1"

# The remedy for a failed toolchain step must name an axl that RUNS. Here the
# bin dir is off PATH and an older axl sits on it (the leftover /usr/bin/axl
# of a .deb install): the bare name ran that one and said "unknown command
# 'toolchain'".
mkdir -p "$WORK/old"
printf '#!/bin/sh\necho "axl: unknown command"; exit 2\n' > "$WORK/old/axl"; chmod +x "$WORK/old/axl"
INSTALL_ARGS=(--toolchain aa64)
run_install "$WORK/hint-shadow.log" PATH="$WORK/bin:$WORK/old:$PATH"
grep -qF "Finish it with:  $WORK/b/axl toolchain install aa64" "$WORK/hint-shadow.log"
check $? "bin dir off PATH, another axl on it: the remedy names $WORK/b/axl"
grep -qF "Skip it next time with:  $WORK/b/axl update --no-toolchain" "$WORK/hint-shadow.log"
check $? "...and so does the skip hint"
# CONTROL: with the bin dir first on PATH the bare name is right, and a full
# path would only be noise.
run_install "$WORK/hint-path.log" PATH="$WORK/b:$WORK/bin:$WORK/old:$PATH"
grep -qF "Finish it with:  axl toolchain install aa64" "$WORK/hint-path.log"
check $? "control: bin dir first on PATH: the remedy says plain 'axl'"
# A relative --bin-dir is printed absolute: the user runs the hint from
# wherever they are, not from the directory the install ran in.
INSTALL_ARGS=(--toolchain aa64 --bin-dir relbin)
( cd "$WORK" && run_install "$WORK/hint-rel.log" PATH="$WORK/bin:$WORK/old:$PATH" )
grep -qF "Finish it with:  $WORK/relbin/axl toolchain install aa64" "$WORK/hint-rel.log"
check $? "relative --bin-dir: the remedy names the absolute $WORK/relbin/axl"
rm -rf "$WORK/relbin"
INSTALL_ARGS=()

# A curl too old for --http1.1 (EL7's 7.29 rejects unknown options) must
# still download: the flag is added only where curl accepts it.
mkdir -p "$WORK/oldcurl"
cat > "$WORK/oldcurl/curl" <<STUB
#!/bin/bash
for a in "\$@"; do
    [[ "\$a" == --http1.1 ]] && { echo "curl: option --http1.1: is unknown" >&2; exit 2; }
done
exec "$WORK/bin/curl" "\$@"
STUB
chmod +x "$WORK/oldcurl/curl"
run_install "$WORK/oldcurl.log" PATH="$WORK/oldcurl:$WORK/bin:$PATH"
rc=$?
[[ "$rc" -eq 0 && -s "$WORK/curl.argv" ]] && ! grep -qF -- '--http1.1' "$WORK/curl.argv"
check $? "curl without --http1.1: install.sh still installs, without the flag (rc=$rc)"
rm -f "$WORK/curl.argv"
PATH="$WORK/oldcurl:$WORK/bin:$PATH" timeout 60 "$PROJECT_DIR/scripts/install-toolchain.sh" \
    aa64 --prefix "$WORK/tcp" > "$WORK/tc-old.log" 2>&1
[[ -s "$WORK/curl.argv" ]] && ! grep -qF -- '--http1.1' "$WORK/curl.argv"
check $? "curl without --http1.1: install-toolchain.sh still reaches the download"

# The hash still bites with the flag ON. If it did not, the flag would be a
# blanket disable rather than a change of trust anchor.
printf 'x' >> "$REL/axl-sdk-linux-$V-x86_64.tar.gz"
run_install "$WORK/corrupt.log" AXL_INSECURE_FETCH=1
rc=$?
[[ "$rc" -ne 0 ]] && grep -qi 'sha256 mismatch' "$WORK/corrupt.log"
check $? "a corrupted asset is still refused with the flag ON (rc=$rc)"

# ---------------------------------------------------------------------------
# install-toolchain.sh: the flag reaches its curl too, and its hint names the
# way out. This is the site where the hash IS pre-shared.
# ---------------------------------------------------------------------------
grep -q 'AXL_INSECURE_FETCH' "$PROJECT_DIR/scripts/install-toolchain.sh"
check $? "install-toolchain.sh honours the flag"

# Its download goes out as HTTP/1.1 too -- this is the fetch that failed. An
# empty --prefix makes it fetch; the stub has no tarball, so it fails after
# recording argv, before anything needs sudo.
rm -f "$WORK/curl.argv"
PATH="$WORK/bin:$PATH" timeout 60 "$PROJECT_DIR/scripts/install-toolchain.sh" \
    aa64 --prefix "$WORK/tcp" > "$WORK/tc-h1.log" 2>&1
[[ -s "$WORK/curl.argv" ]] && ! grep -vqE '(^| )--http1\.1( |$)' "$WORK/curl.argv"
check $? "install-toolchain.sh: the toolchain download carries --http1.1"

grep -q 'AXL_INSECURE_FETCH' "$PROJECT_DIR/scripts/axl-toolchains.conf" 2>/dev/null || true
grep -qE 'AXL_(AA64|X64)_TOOLCHAIN_SHA256=' "$PROJECT_DIR/scripts/axl-toolchains.conf"
check $? "the toolchain's expected hash ships in the SDK (pre-shared, not fetched)"

# A user whose fetch dies at TLS must be told both ways out, or they do the
# per-machine CA setup this exists to avoid.
grep -qi 'AXL_INSECURE_FETCH' "$PROJECT_DIR/scripts/install-toolchain.sh" \
  && grep -qiE 'certificate|CA' "$PROJECT_DIR/scripts/install-toolchain.sh"
check $? "a failed toolchain fetch names both the CA and the flag"

# ---------------------------------------------------------------------------
# The contract is documented where the trust model lives.
# ---------------------------------------------------------------------------
grep -q 'AXL_INSECURE_FETCH' "$PROJECT_DIR/docs/AXL-Distribution-Design.md"
check $? "the design doc records the knob"
grep -q 'AXL_INSECURE_FETCH' "$PROJECT_DIR/packaging/install.sh"
check $? "install.sh --help mentions it"

# ---------------------------------------------------------------------------
# AN ALREADY-INSTALLED TOOLCHAIN MUST NOT FETCH AT ALL.
#
# This is why AXL_INSECURE_FETCH is dormant for most users: the TLS failure it
# works around only happens on a machine that does not yet have the pinned
# toolchain. Worth pinning, because the failure mode if it regresses is silent
# and expensive -- every run would re-download 239 MB (x64) or 500 MB (aa64),
# and every run behind a MITM proxy would start failing at TLS again.
#
# Note what "already installed" means: the binary exists AND reports the
# version pinned in axl-toolchains.conf. So the next toolchain BUMP correctly
# makes everyone download again -- dormant is not the same as gone.
# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
for _arch in aa64 x64; do
    case "$_arch" in
        aa64) _dir="$AXL_AA64_TOOLCHAIN_DIR" ;;
        x64)  _dir="$AXL_X64_TOOLCHAIN_DIR" ;;
    esac
    if [[ ! -d "$_dir" ]]; then
        test_host_pass "SKIP: $_arch toolchain not installed -- cannot test the no-fetch path"
        continue
    fi
    rm -f "$WORK/curl.argv"
    PATH="$WORK/bin:$PATH" timeout 120 "$PROJECT_DIR/scripts/install-toolchain.sh" \
        "$_arch" > "$WORK/tc-$_arch.log" 2>&1
    _rc=$?
    # `wc -l < f 2>/dev/null` does NOT silence a missing file: the redirect is
    # performed by the SHELL, and 2>/dev/null applies to wc. Test first.
    if [[ -f "$WORK/curl.argv" ]]; then _n=$(wc -l < "$WORK/curl.argv"); else _n=0; fi
    [[ "$_rc" -eq 0 && "$_n" -eq 0 ]]
    check $? "$_arch already installed: exits 0 and fetches nothing (rc=$_rc, ${_n} curl call(s))"
done

test_host_summary "insecure-fetch"
