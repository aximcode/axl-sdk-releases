#!/bin/bash
# test-meta: arch=x64 needs= est=13 local-only=0
# test-mkfixture-post-qemu.sh -- end-to-end test for mkfixture's HTTP
# write target (HF2.4): a disk-less / net-only capture that POSTs the
# whole fixture as an in-memory ustar tarball to a collector.
#
# Runs a tiny host HTTP collector, boots mkfixture in URL mode with
# --net (and --bridges/--gpu so the device manifests are non-trivial),
# and verifies the guest POSTed a valid ustar tarball whose members are
# the captured artifacts (pci.json / cpu.json / manifest.json / etc.),
# with manifest.json reporting fixture_format HF2.3.
#
# The guest reaches the host via QEMU user-mode networking's gateway
# (10.0.2.2). Auxiliary; opts out of the test-axl.sh ratchet. x86-only.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
MKFIXTURE="$PROJECT_DIR/out/native-x64/tools/mkfixture.efi"
# This test drives run-qemu.sh directly (no common-test.sh), so it resolves its
# own host port rather than via the test_port helper. An explicit
# TEST_PORT_BASE still wins; otherwise claim one that is verified free now and
# hold it for this shell's lifetime, so a second independent run of the suite
# cannot land the collector on the same port.
if [[ -n "${TEST_PORT_BASE:-}" ]]; then
    PORT=$(( TEST_PORT_BASE + 0 ))
else
    source "$PROJECT_DIR/scripts/axl-common.sh"
    axl_alloc_host_port PORT || exit 1
fi

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

if [[ ! -x "$MKFIXTURE" ]]; then
    echo "Building tools..."
    make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$MKFIXTURE" ]] || { echo "FAIL: mkfixture.efi not found at $MKFIXTURE"; exit 1; }

TARBALL="$(mktemp)"; rm -f "$TARBALL"
COLLECTOR="$(mktemp --suffix=.py)"
cat > "$COLLECTOR" <<'PY'
import http.server, sys
out = sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        data = self.rfile.read(n)
        with open(out, 'wb') as f:
            f.write(data)
        self.send_response(200); self.end_headers(); self.wfile.write(b'ok')
    def log_message(self, *a):
        pass
http.server.HTTPServer(('0.0.0.0', int(sys.argv[1])), H).serve_forever()
PY

python3 "$COLLECTOR" "$PORT" "$TARBALL" &
COLL_PID=$!
trap 'kill "$COLL_PID" 2>/dev/null || true; rm -f "$TARBALL" "$COLLECTOR"' EXIT
sleep 1

echo "=== mkfixture URL mode → POST tarball to host collector ==="
POST_OUT=$(timeout 120s "$RUN_QEMU" --net --bridges --gpu \
    "$MKFIXTURE" "http://10.0.2.2:${PORT}/fixtures/test" 2>&1 || true)

if grep -qE "POSTed .* tarball .* \(HTTP 2[0-9][0-9]\)" <<< "$POST_OUT"; then
    pass "mkfixture reports a successful POST (HTTP 2xx)"
else
    fail "mkfixture did not report a successful POST" \
         "$(grep -iE 'mkfixture:|POST|fail' <<< "$POST_OUT" | head)"
fi

# Give the collector a moment to flush, then stop it.
sleep 1
kill "$COLL_PID" 2>/dev/null || true

if [[ -f "$TARBALL" && -s "$TARBALL" ]]; then
    pass "collector received a non-empty body ($(stat -c%s "$TARBALL") bytes)"
else
    fail "collector received no tarball"
fi

# HF2.4 POSTs a gzipped tarball (application/gzip): the body must be a
# valid gzip stream that host gzip accepts.
if [[ -s "$TARBALL" ]] && gzip -t "$TARBALL" 2>/dev/null; then
    pass "POSTed body is a valid gzip stream (host gzip -t)"
else
    fail "POSTed body is not valid gzip" \
         "$(head -c2 "$TARBALL" 2>/dev/null | od -An -tx1)"
fi

# Decompressed, it must be a valid ustar archive whose members are the
# captured manifests, with manifest.json reporting the HF2.3 format.
# python's tarfile.open auto-detects the gzip wrapper.
if [[ -s "$TARBALL" ]] && python3 - "$TARBALL" <<'PYEOF'
import json, tarfile, sys
t = tarfile.open(sys.argv[1])           # raises if not a valid tar
names = {m.name for m in t.getmembers()}
required = {"smbios.bin", "cpu.json", "pci.json", "manifest.json"}
missing = required - names
assert not missing, f"tarball missing {missing}; have {sorted(names)}"
mf = json.loads(t.extractfile("manifest.json").read())
assert mf["fixture_format"] == "HF2.3", f"fixture_format {mf.get('fixture_format')!r}"
assert any(n.startswith("acpi/") for n in names), "no acpi/ members"
PYEOF
then
    pass "POSTed body is valid ustar with the captured manifests (HF2.3)"
else
    fail "POSTed body is not a valid fixture tarball" \
         "$(python3 -c "import tarfile,sys; print([m.name for m in tarfile.open(sys.argv[1]).getmembers()])" "$TARBALL" 2>&1 | head)"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
