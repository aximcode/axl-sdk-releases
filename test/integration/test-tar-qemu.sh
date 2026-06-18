#!/bin/bash
# test-meta: arch=x64 needs= est=71 local-only=0
# test-tar-qemu.sh -- end-to-end test for tools/tar.efi over virtiofs.
#
# Exercises the ustar tool against a virtiofs-mounted host directory:
#   1. create an archive from a directory tree (recursive walk), and
#      confirm it is valid ustar that host python/tar can read with the
#      right members + content (writer + the axl_dir_walk separator fix);
#   2. list it (-t);
#   3. extract our own archive (-x) and confirm the tree round-trips;
#   4. extract a host-made GNU tar archive and confirm the reader handles
#      standard '/'-separated names (interop + path sanitize).
#
# Auxiliary; opts out of the test-axl.sh ratchet. x86-only (virtiofs
# --mount depends on OVMF VirtioFsDxe; aa64 OVMF lacks it) — mirrors
# test-mkfixture-qemu.sh / test-spd-qemu.sh policy.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
TAR="$PROJECT_DIR/out/native-x64/tools/tar.efi"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

if [[ ! -x "$TAR" ]]; then
    echo "Building tools..."
    make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$TAR" ]] || { echo "FAIL: tar.efi not found at $TAR"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Source tree to archive: a top file + a nested subdirectory.
mkdir -p "$WORK/src/sub"
printf 'top-content\n'    > "$WORK/src/probe.txt"
printf 'nested-content\n' > "$WORK/src/sub/b.txt"

# ----------------------------------------------------------------------
# 1. Create: tar -c a.tar src   (recursive directory walk)
# ----------------------------------------------------------------------
echo "=== create: tar -c FS1:\\a.tar FS1:\\src ==="
timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -c 'FS1:\a.tar' 'FS1:\src' > /dev/null 2>&1 || true

if [[ -f "$WORK/a.tar" ]]; then
    pass "tar -c produced an archive"
else
    fail "tar -c produced no archive"
fi

# The archive must be valid ustar that the host's python tarfile reads,
# with the file members + exact contents (proves the writer AND the
# axl_dir_walk separator fix — a mixed-separator path would have failed
# to open during the walk, dropping the member).
if [[ -f "$WORK/a.tar" ]] && python3 - "$WORK/a.tar" <<'PYEOF'
import tarfile, sys
t = tarfile.open(sys.argv[1])
members = {m.name.replace("\\", "/"): m for m in t.getmembers()}
# names carry the guest path prefix; match on the tail.
def find(suffix):
    for k, m in members.items():
        if k.endswith(suffix):
            return m
    raise AssertionError(f"no member ending {suffix!r}; have {list(members)}")
probe = find("src/probe.txt")
b     = find("src/sub/b.txt")
assert t.extractfile(probe).read() == b"top-content\n", "probe.txt content"
assert t.extractfile(b).read() == b"nested-content\n", "b.txt content"
assert any(m.isdir() and m.name.replace("\\","/").endswith("src")
           for m in t.getmembers()), "src dir entry present"
PYEOF
then
    pass "archive is valid ustar; members + content round-trip (writer + walk fix)"
else
    fail "archive not valid ustar or members/content wrong" \
         "$(tar -tvf "$WORK/a.tar" 2>&1 | head)"
fi

# Host GNU tar must also be able to list it (independent interop check).
if [[ -f "$WORK/a.tar" ]] && tar -tf "$WORK/a.tar" >/dev/null 2>&1; then
    pass "host GNU tar lists our archive (ustar interop)"
else
    fail "host GNU tar could not read our archive"
fi

# ----------------------------------------------------------------------
# 2. List: tar -t a.tar
# ----------------------------------------------------------------------
echo "=== list: tar -t FS1:\\a.tar ==="
LIST_OUT=$(timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -t 'FS1:\a.tar' 2>&1 || true)
if grep -q 'probe.txt' <<< "$LIST_OUT" && grep -q 'b.txt' <<< "$LIST_OUT"; then
    pass "tar -t lists both members"
else
    fail "tar -t did not list members" "$LIST_OUT"
fi

# ----------------------------------------------------------------------
# 3. Extract our own archive: tar -x a.tar -C out  (round-trip)
# ----------------------------------------------------------------------
echo "=== extract: tar -x FS1:\\a.tar -C FS1:\\out ==="
timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -x 'FS1:\a.tar' -C 'FS1:\out' > /dev/null 2>&1 || true

if [[ "$(cat "$WORK/out/src/probe.txt" 2>/dev/null)" == "top-content" ]] \
   && [[ "$(cat "$WORK/out/src/sub/b.txt" 2>/dev/null)" == "nested-content" ]]; then
    pass "tar -x round-trips the tree (FS prefix stripped, dirs recreated)"
else
    fail "tar -x round-trip mismatch" \
         "$(find "$WORK/out" -type f 2>/dev/null)"
fi

# ----------------------------------------------------------------------
# 4. Extract a host-made GNU tar (standard '/'-separated relative names)
# ----------------------------------------------------------------------
echo "=== extract GNU archive: tar -x FS1:\\gnu.tar -C FS1:\\out2 ==="
mkdir -p "$WORK/g/d"
printf 'g-top\n'  > "$WORK/g/g.txt"
printf 'g-deep\n' > "$WORK/g/d/e.txt"
tar -cf "$WORK/gnu.tar" -C "$WORK/g" g.txt d/e.txt
timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -x 'FS1:\gnu.tar' -C 'FS1:\out2' > /dev/null 2>&1 || true

if [[ "$(cat "$WORK/out2/g.txt" 2>/dev/null)" == "g-top" ]] \
   && [[ "$(cat "$WORK/out2/d/e.txt" 2>/dev/null)" == "g-deep" ]]; then
    pass "tar -x reads a standard GNU ustar archive (interop + path sanitize)"
else
    fail "extracting GNU archive failed" \
         "$(find "$WORK/out2" -type f 2>/dev/null)"
fi

# ----------------------------------------------------------------------
# 5. Create gzipped: tar -c -z a.tgz src
#    The output must be a real gzip stream that host gzip/tar accept
#    (outbound interop), and its members must round-trip.
# ----------------------------------------------------------------------
echo "=== create gzip: tar -c -z FS1:\\a.tgz FS1:\\src ==="
timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -c -z 'FS1:\a.tgz' 'FS1:\src' > /dev/null 2>&1 || true

if [[ -f "$WORK/a.tgz" ]] && gzip -t "$WORK/a.tgz" 2>/dev/null; then
    pass "tar -c -z produced a valid gzip stream (host gzip -t)"
else
    fail "tar -c -z output is not a valid gzip stream" \
         "$(head -c2 "$WORK/a.tgz" 2>/dev/null | od -An -tx1)"
fi

# Host GNU tar must list our .tar.gz members (gzip interop). Capture the
# listing first, then grep — the member names are what we're checking,
# not GNU tar's exit code (under `set -o pipefail` a piped `tar -tzf`
# would couple this to tar's exit status; with the 10240-record padding
# in AxlTar that exit is clean, but the capture keeps the assertion about
# listability, matching the `tar -xzO` check below).
if [[ -f "$WORK/a.tgz" ]]; then
    names="$(tar -tzf "$WORK/a.tgz" 2>/dev/null || true)"
    if grep -q 'probe.txt' <<< "$names" && grep -q 'b.txt' <<< "$names"; then
        pass "host 'tar -tzf' lists members of our .tar.gz (gzip interop out)"
    else
        fail "host tar could not list our gzipped archive" "$names"
    fi
fi

# Recover member CONTENT via host tar to stdout (-O), which proves the
# gzip+ustar payload round-trips through GNU tar without depending on
# UEFI-style member names (FS1:\...\ — backslash/volume-prefixed — which
# don't map onto a POSIX nested tree; that's an orthogonal tar name
# concern, not a gzip one).
if [[ -f "$WORK/a.tgz" ]]; then
    content="$(tar -xzOf "$WORK/a.tgz" 2>/dev/null)"
    if grep -q 'top-content' <<< "$content" \
       && grep -q 'nested-content' <<< "$content"; then
        pass "host 'tar -xzO' recovers member content from our .tar.gz"
    else
        fail "host tar could not recover content from our gzipped archive" \
             "$content"
    fi
fi

# ----------------------------------------------------------------------
# 6. Extract a host-made .tar.gz with auto-detection (NO -z flag).
#    `tar -x` must sniff the 1f 8b magic and transparently inflate, so
#    a .tar.gz "just works" (inbound interop + auto-detect).
# ----------------------------------------------------------------------
echo "=== extract host .tar.gz w/ auto-detect: tar -x FS1:\\host.tgz ==="
tar -czf "$WORK/host.tgz" -C "$WORK/g" g.txt d/e.txt
timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -x 'FS1:\host.tgz' -C 'FS1:\out3' > /dev/null 2>&1 || true

if [[ "$(cat "$WORK/out3/g.txt" 2>/dev/null)" == "g-top" ]] \
   && [[ "$(cat "$WORK/out3/d/e.txt" 2>/dev/null)" == "g-deep" ]]; then
    pass "tar -x auto-detects gzip (1f 8b) and inflates a host .tar.gz"
else
    fail "tar -x did not auto-detect/extract a host-made .tar.gz" \
         "$(find "$WORK/out3" -type f 2>/dev/null)"
fi

# ----------------------------------------------------------------------
# 7. Full gzip round-trip through the guest: -c -z then -x (auto).
# ----------------------------------------------------------------------
echo "=== gzip round-trip: -c -z then -x FS1:\\a.tgz ==="
if [[ -f "$WORK/a.tgz" ]]; then
timeout 60s "$RUN_QEMU" --mount "$WORK" \
    "$TAR" -x 'FS1:\a.tgz' -C 'FS1:\out4' > /dev/null 2>&1 || true
fi
if [[ "$(cat "$WORK/out4/src/probe.txt" 2>/dev/null)" == "top-content" ]] \
   && [[ "$(cat "$WORK/out4/src/sub/b.txt" 2>/dev/null)" == "nested-content" ]]; then
    pass "guest -x round-trips a guest-created .tar.gz (auto-detect)"
else
    fail "guest could not round-trip its own .tar.gz"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
