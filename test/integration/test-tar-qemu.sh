#!/bin/bash
# test-meta: arch=x64 needs= est=71 local-only=0 guest=none
# guest=none: --mount: a host directory as a virtio-fs volume of its own,
#   fixed when QEMU starts.
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
TAR="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/tools/tar.efi"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

if [[ ! -x "$TAR" ]]; then
    echo "Building tools..."
    "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$TAR" ]] || { echo "FAIL: tar.efi not found at $TAR"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Source tree to archive: a top file + a nested subdirectory.
mkdir -p "$WORK/src/sub"
printf 'top-content\n'    > "$WORK/src/probe.txt"
printf 'nested-content\n' > "$WORK/src/sub/b.txt"

# ----------------------------------------------------------------------
# ONE BOOT, seven tar invocations.
#
# This was seven separate `run-qemu.sh` calls -- seven guest boots for 50 s of
# wall clock, of which ~49 s was firmware. Nothing here needed the isolation:
# every boot mounted the SAME host directory and differed only in tar.efi's
# arguments, and the scenarios are already COUPLED through that mount (step 2
# lists what step 1 wrote; step 7 extracts what step 5 wrote). A single shell
# session running them in order preserves those dependencies exactly, because
# the dependency was never the boot -- it was the filesystem.
#
# The two host-made archives are prepared here rather than mid-sequence, which
# is the only ordering the merge actually changes. Both are inputs the guest
# only reads.
#
# What this gives up, stated rather than discovered later: a guest-side failure
# in an early step now leaves the later ones running against missing inputs, so
# one real fault can print several failures. The step markers below are what
# keeps that diagnosable -- each assertion still names its own step, and the
# transcript is sliced per step rather than grepped as a whole.
# See AXL-CI-Release-Speed-Design.md §12.13.
# ----------------------------------------------------------------------
mkdir -p "$WORK/g/d"
printf 'g-top\n'  > "$WORK/g/g.txt"
printf 'g-deep\n' > "$WORK/g/d/e.txt"
tar -cf  "$WORK/gnu.tar"  -C "$WORK/g" g.txt d/e.txt
tar -czf "$WORK/host.tgz" -C "$WORK/g" g.txt d/e.txt

# Step 8's tree. Member names are relative with '/' (as GNU tar and bsdtar
# store them), so AxlTar's ustar name/prefix split carries a path past the
# 100-byte name field. The tree pins both sides of that boundary:
#   - e/sub/<120 x d>/ is a DIRECTORY whose one component is over 100 bytes:
#     no split can hold it, so it is reported and left out -- never cut;
#   - its child x.txt (a 132-byte path, split at its 126-byte prefix) IS
#     archived. The walk
#     reaches it only AFTER its directory failed, whatever order readdir
#     returns, so a walk that stops at the first failure is caught
#     independently of order;
#   - e/t/<90 x f>/<90 x f>/ (186 bytes) splits and is archived, but its
#     child <90 x f>.txt makes a 280-byte path that no split fits: reported.
LONG_D=$(printf 'd%.0s' $(seq 1 120))
LONG_F=$(printf 'f%.0s' $(seq 1 90))
mkdir -p "$WORK/e/sub/$LONG_D" "$WORK/e/t/$LONG_F/$LONG_F"
printf 'e1\n' > "$WORK/e/e1.txt"
printf 'e2\n' > "$WORK/e/sub/e2.txt"
printf 'xx\n' > "$WORK/e/sub/$LONG_D/x.txt"
printf 'ff\n' > "$WORK/e/t/$LONG_F/$LONG_F/$LONG_F.txt"

# Step 9's archive: the member spelling tar.efi wrote before it built
# relative '/' names -- the raw UEFI path, volume prefix and '\'. Archives
# made by older tar.efi builds must still extract (sanitize_name strips it).
python3 - "$WORK/old.tar" <<'OLDEOF'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.USTAR_FORMAT) as t:
    d = tarfile.TarInfo("FS1:\\old")
    d.type = tarfile.DIRTYPE
    d.mode = 0o755
    t.addfile(d)
    data = b"old-format\n"
    f = tarfile.TarInfo("FS1:\\old\\o.txt")
    f.size = len(data)
    t.addfile(f, io.BytesIO(data))
OLDEOF

# Steps 10-12: REAL host archives with a name over 100 bytes. The path
# lg/<147 bytes> has no usable split (its one component is over 100), so
# GNU tar's default `gnu` format carries it in an 'L' record and
# `--format=pax` in an 'x' record's `path`; either way the member's own
# ustar header holds a CUT name. A reader that ignores the record writes
# the member under that cut name -- so the destination is SEEDED with a
# file of exactly the cut name (lg/ + the first 97 bytes), which must be
# left untouched.
LONG_N="$(printf 'n%.0s' $(seq 1 143)).txt"
CUT_N="${LONG_N:0:97}"
mkdir -p "$WORK/lsrc/lg"
printf 'long-content\n' > "$WORK/lsrc/lg/$LONG_N"
tar --format=gnu -cf "$WORK/lgnu.tar" -C "$WORK/lsrc" lg
tar --format=pax -cf "$WORK/lpax.tar" -C "$WORK/lsrc" lg
for d in out6 out7; do
    mkdir -p "$WORK/$d/lg"
    printf 'seed\n' > "$WORK/$d/lg/$CUT_N"
done

# Step 13: links. A symlink and a hardlink are reported and not
# extracted (no link is created, and none becomes an empty file).
mkdir -p "$WORK/ksrc/lk"
printf 'real\n' > "$WORK/ksrc/lk/real.txt"
ln -s real.txt "$WORK/ksrc/lk/sym"
ln "$WORK/ksrc/lk/real.txt" "$WORK/ksrc/lk/hard"
tar -cf "$WORK/links.tar" -C "$WORK/ksrc" lk/real.txt lk/sym lk/hard

# Step 14: a ':' left in a sanitized name (a volume spelled mid-path) is
# refused, never handed to a path parser that might read it as a volume.
python3 - "$WORK/colon.tar" <<'COLEOF'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.USTAR_FORMAT) as t:
    for name, data in (("ok/fine.txt", b"fine\n"), ("ok/fs1:/evil.txt", b"evil\n")):
        f = tarfile.TarInfo(name)
        f.size = len(data)
        t.addfile(f, io.BytesIO(data))
COLEOF

# Step 15: a REAL host archive cut off inside a member's data. The
# member must be reported and NOT left behind as a silently short file.
mkdir -p "$WORK/tsrc/tr"
head -c 3000 /dev/zero | tr '\0' 'b' > "$WORK/tsrc/tr/big.bin"
tar -cf "$WORK/trunc-full.tar" -C "$WORK/tsrc" tr/big.bin
head -c 1512 "$WORK/trunc-full.tar" > "$WORK/trunc.tar"   # header + 1000 B
# Safe writes: the destination already holds tr/big.bin. A truncated
# member must leave it exactly as it was -- never emptied, never short.
mkdir -p "$WORK/out10/tr"
printf 'original content, must survive\n' > "$WORK/out10/tr/big.bin"
cp "$WORK/out10/tr/big.bin" "$WORK/big.orig"

# Steps 16-23: a REAL host archive -- a.txt (1000 B), b.txt (3 B), GNU
# format -- cut at each place a stream can stop. Layout: a.txt header at
# 0, data 512..1512, padding to 1536; b.txt header 1536..2048. GNU tar
# reports "Unexpected EOF" (exit 2) for a cut inside data (700) or padding
# (1520); a cut inside a header (1700) it accepts silently, losing b.txt --
# libarchive reports it, and so do we. A cut AT a header boundary (1536)
# is a clean end for both: the control that proves the check can pass.
mkdir -p "$WORK/csrc"
head -c 1000 /dev/zero | tr '\0' 'a' > "$WORK/csrc/a.txt"
printf 'bb\n' > "$WORK/csrc/b.txt"
tar --format=gnu -cf "$WORK/cut-full.tar" -C "$WORK/csrc" a.txt b.txt
for c in 700 1520 1700 1536; do
    head -c "$c" "$WORK/cut-full.tar" > "$WORK/cut$c.tar"
    mkdir -p "$WORK/xc$c"
done
# ...and a whole member DOES replace an existing file (no temp left).
printf 'stale\n' > "$WORK/xc1536/a.txt"

# Step 24: members tar.efi cannot put in place, each refused with its
# reason, and names near tar's own temporaries, which are NOT refused.
#   dirhere.txt      -- a directory is already there;
#   ro.txt           -- a read-only file is already there (mode 0444,
#                       which virtiofs reports as EFI_FILE_READ_ONLY);
#   ~TAR12~1.TMP     -- shaped like a temporary's FAT 8.3 alias: virtiofs
#                       has no aliases, so it extracts like any name (the
#                       FAT collision is test-tar-fat-qemu.sh's);
#   ~tar1234abcd.tmp -- shaped like a temporary's long name: extracts;
#   ok.txt           -- extracted, to show the run went on.
python3 - "$WORK/odd.tar" <<'ODDEOF'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.USTAR_FORMAT) as t:
    for name in ("dirhere.txt", "ro.txt", "~TAR12~1.TMP", "~tar1234abcd.tmp", "ok.txt"):
        data = name.encode() + b"\n"
        f = tarfile.TarInfo(name)
        f.size = len(data)
        t.addfile(f, io.BytesIO(data))
ODDEOF
mkdir -p "$WORK/outR/dirhere.txt"
printf 'keep\n' > "$WORK/outR/ro.txt"
chmod 0444 "$WORK/outR/ro.txt"

NSH="$WORK/tar.nsh"
{
    echo 'fs0:'
    echo 'echo TARSTEP:1'
    echo 'tar.efi -c FS1:\a.tar FS1:\src'
    echo 'echo TARSTEP:2'
    echo 'tar.efi -t FS1:\a.tar'
    echo 'echo TARSTEP:3'
    echo 'tar.efi -x FS1:\a.tar -C FS1:\out'
    echo 'echo TARSTEP:4'
    echo 'tar.efi -x FS1:\gnu.tar -C FS1:\out2'
    echo 'echo TARSTEP:5'
    echo 'tar.efi -c -z FS1:\a.tgz FS1:\src'
    echo 'echo TARSTEP:6'
    echo 'tar.efi -x FS1:\host.tgz -C FS1:\out3'
    echo 'echo TARSTEP:7'
    echo 'tar.efi -x FS1:\a.tgz -C FS1:\out4'
    echo 'echo TARSTEP:8'
    echo 'tar.efi -c FS1:\e.tar FS1:\e'
    echo 'echo TARRC8=%lasterror%'
    echo 'echo TARSTEP:9'
    echo 'tar.efi -x FS1:\old.tar -C FS1:\out5'
    echo 'echo TARSTEP:10'
    echo 'tar.efi -x FS1:\lgnu.tar -C FS1:\out6'
    echo 'echo TARRC10=%lasterror%'
    echo 'echo TARSTEP:11'
    echo 'tar.efi -x FS1:\lpax.tar -C FS1:\out7'
    echo 'echo TARRC11=%lasterror%'
    echo 'echo TARSTEP:12'
    echo 'tar.efi -t FS1:\lpax.tar'
    echo 'echo TARSTEP:13'
    echo 'tar.efi -x FS1:\links.tar -C FS1:\out8'
    echo 'echo TARRC13=%lasterror%'
    echo 'echo TARSTEP:14'
    echo 'tar.efi -x FS1:\colon.tar -C FS1:\out9'
    echo 'echo TARRC14=%lasterror%'
    echo 'echo TARSTEP:15'
    echo 'tar.efi -x FS1:\trunc.tar -C FS1:\out10'
    echo 'echo TARRC15=%lasterror%'
    echo 'echo TARSTEP:24'
    echo 'tar.efi -x FS1:\odd.tar -C FS1:\outR'
    echo 'echo TARRC24=%lasterror%'
    step=16
    for c in 700 1520 1700 1536; do
        echo "echo TARSTEP:$step"
        echo "tar.efi -t FS1:\\cut$c.tar"
        echo "echo TARRC$step=%lasterror%"
        step=$((step + 1))
        echo "echo TARSTEP:$step"
        echo "tar.efi -x FS1:\\cut$c.tar -C FS1:\\xc$c"
        echo "echo TARRC$step=%lasterror%"
        step=$((step + 1))
    done
    echo 'echo TARSTEP:END'
} > "$NSH"

echo "=== one boot: 24 tar invocations over virtiofs ==="
GUEST_LOG="$WORK/guest.log"
timeout 200s "$RUN_QEMU" --mount "$WORK" --nsh "$NSH" "$TAR" \
    > "$GUEST_LOG" 2>&1 || true
tr -d '\r' < "$GUEST_LOG" > "$WORK/guest.clean"

# The guest must have reached the end; without this a boot that died after
# step 1 would be reported as six independent tar bugs.
if grep -q 'TARSTEP:END' "$WORK/guest.clean"; then
    pass "the guest ran all 24 tar invocations to completion"
else
    fail "the guest did not reach TARSTEP:END — later assertions are suspect" \
         "$(tail -15 "$WORK/guest.clean")"
fi

# Slice one step's output out of the single transcript.
#
# The shell echoes the command BEFORE its output, so the transcript carries
# `FS0:\> echo TARSTEP:2` and then `TARSTEP:2` on the next line. A sed range
# ending at the next /TARSTEP:/ therefore closes on the echoed marker itself
# and yields one line -- which read as "tar -t listed nothing", a guest bug that
# was not there. Start AFTER the bare marker line, stop at the next command
# echo. POSIX awk only (mawk in CI; see check-awk-portability).
step_out() {
    awk -v m="TARSTEP:$1" '
        $0 == m            { on = 1; next }
        on && /echo TARSTEP:/ { exit }
        on && /^TARSTEP:/     { exit }
        on                 { print }
    ' "$WORK/guest.clean"
}

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
probe = t.getmember("src/probe.txt")
b     = t.getmember("src/sub/b.txt")
assert t.extractfile(probe).read() == b"top-content\n", "probe.txt content"
assert t.extractfile(b).read() == b"nested-content\n", "b.txt content"
assert t.getmember("src").isdir(), "src dir entry present"
PYEOF
then
    pass "archive is valid ustar; members + content round-trip (writer + walk fix)"
else
    fail "archive not valid ustar or members/content wrong" \
         "$(tar -tvf "$WORK/a.tar" 2>&1 | head)"
fi

# Host GNU tar must list it with the names GNU tar itself would store for
# the same tree: relative, '/'-separated, no volume, directories ending '/'.
a_names="$(tar -tf "$WORK/a.tar" 2>/dev/null | LC_ALL=C sort || true)"
a_want="$(printf '%s\n' src/ src/probe.txt src/sub/ src/sub/b.txt | LC_ALL=C sort)"
if [[ "$a_names" == "$a_want" ]]; then
    pass "host GNU tar lists our archive with relative '/' names"
else
    fail "host GNU tar listing differs" \
         "want:"$'\n'"$a_want"$'\n'"got:"$'\n'"$a_names"
fi

# ...and extracts it into the same tree, byte for byte, with nothing else.
mkdir -p "$WORK/hostx"
if tar -xf "$WORK/a.tar" -C "$WORK/hostx" 2>/dev/null \
   && diff -r "$WORK/src" "$WORK/hostx/src" >/dev/null \
   && [[ "$(ls -A "$WORK/hostx")" == "src" ]]; then
    pass "host GNU tar extracts our archive into the original tree"
else
    fail "host GNU tar did not extract our archive into the original tree" \
         "$(cd "$WORK/hostx" && find . | LC_ALL=C sort)"
fi

# ----------------------------------------------------------------------
# 2. List: tar -t a.tar
# ----------------------------------------------------------------------
echo "=== list: tar -t FS1:\\a.tar ==="
LIST_OUT="$(step_out 2)"
if grep -q 'probe.txt' <<< "$LIST_OUT" && grep -q 'b.txt' <<< "$LIST_OUT"; then
    pass "tar -t lists both members"
else
    fail "tar -t did not list members" "$LIST_OUT"
fi

# ----------------------------------------------------------------------
# 3. Extract our own archive: tar -x a.tar -C out  (round-trip)
# ----------------------------------------------------------------------
echo "=== extract: tar -x FS1:\\a.tar -C FS1:\\out ==="
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
if [[ "$(cat "$WORK/out4/src/probe.txt" 2>/dev/null)" == "top-content" ]] \
   && [[ "$(cat "$WORK/out4/src/sub/b.txt" 2>/dev/null)" == "nested-content" ]]; then
    pass "guest -x round-trips a guest-created .tar.gz (auto-detect)"
else
    fail "guest could not round-trip its own .tar.gz"
fi

# ----------------------------------------------------------------------
# 8. One member that cannot be archived, two levels down: reported, the
#    rest archived, and a non-zero exit -- GNU tar's behaviour.
# ----------------------------------------------------------------------
echo "=== create past an unarchivable member: tar -c FS1:\\e.tar FS1:\\e ==="
E_OUT="$(step_out 8)"
e_names="$(tar -tf "$WORK/e.tar" 2>/dev/null | LC_ALL=C sort || true)"
e_want="$(printf '%s\n' e/ e/e1.txt e/sub/ e/sub/e2.txt "e/sub/$LONG_D/x.txt" \
          e/t/ "e/t/$LONG_F/" "e/t/$LONG_F/$LONG_F/" | LC_ALL=C sort)"
if [[ "$e_names" == "$e_want" ]]; then
    pass "tar -c archives exactly the members ustar can hold, whatever the walk order"
else
    fail "tar -c member list differs from the expected set" \
         "want:"$'\n'"$e_want"$'\n'"got:"$'\n'"$e_names"
fi
if grep -q "^tar: cannot archive dir .*$LONG_D\$" <<< "$E_OUT" \
   && grep -q "^tar: cannot archive .*$LONG_F.$LONG_F.$LONG_F\.txt (name too long?)\$" <<< "$E_OUT" \
   && [[ "$(grep -c '^tar: cannot archive' <<< "$E_OUT")" == 2 ]] \
   && grep -qx 'TARRC8=0x1' <<< "$E_OUT"; then
    pass "tar -c reports exactly the two members it cannot hold and exits 0x1"
else
    fail "tar -c did not report the two members or exit non-zero" "$E_OUT"
fi

# ----------------------------------------------------------------------
# 9. An archive in the OLD member spelling ("FS1:\old\o.txt") still extracts.
# ----------------------------------------------------------------------
echo "=== extract old-format names: tar -x FS1:\\old.tar -C FS1:\\out5 ==="
if [[ "$(cat "$WORK/out5/old/o.txt" 2>/dev/null)" == "old-format" ]] \
   && [[ "$(cd "$WORK/out5" && find . | LC_ALL=C sort)" == $'.\n./old\n./old/o.txt' ]]; then
    pass "tar -x extracts an old-format archive (volume and '\\' stripped)"
else
    fail "tar -x could not extract an old-format archive" \
         "$(step_out 9; find "$WORK/out5" 2>/dev/null)"
fi

# ----------------------------------------------------------------------
# 10-11. Host archives with a long name, GNU 'L' and pax 'x': extracted
#        under the FULL name, the seeded cut-name file untouched, nothing
#        else written (no @LongLink, no PaxHeaders), exit 0.
# ----------------------------------------------------------------------
check_long_extract() {   # <step> <outdir> <format>
    local out="$WORK/$2" want
    want="$(printf '%s\n' . ./lg "./lg/$CUT_N" "./lg/$LONG_N" | LC_ALL=C sort)"
    if [[ "$(cat "$out/lg/$LONG_N" 2>/dev/null)" == "long-content" ]] \
       && [[ "$(cat "$out/lg/$CUT_N" 2>/dev/null)" == "seed" ]] \
       && [[ "$(cd "$out" && find . | LC_ALL=C sort)" == "$want" ]] \
       && grep -qx "TARRC$1=0x0" <<< "$(step_out "$1")"; then
        pass "tar -x extracts a host --format=$3 long name whole; the cut-name file is untouched"
    else
        fail "tar -x mishandled a host --format=$3 long name" \
             "$(step_out "$1"; cd "$out" && find . | LC_ALL=C sort)"
    fi
}
echo "=== host GNU/pax long names: tar -x FS1:\\lgnu.tar / FS1:\\lpax.tar ==="
check_long_extract 10 out6 gnu
check_long_extract 11 out7 pax

# 12. tar -t lists the long name whole.
if grep -qx "lg/$LONG_N" <<< "$(step_out 12)"; then
    pass "tar -t lists a pax long name whole"
else
    fail "tar -t did not list the long name whole" "$(step_out 12)"
fi

# ----------------------------------------------------------------------
# 13. A symlink and a hardlink: reported, not extracted, exit 1.
# ----------------------------------------------------------------------
echo "=== links: tar -x FS1:\\links.tar -C FS1:\\out8 ==="
L_OUT="$(step_out 13)"
if [[ "$(cd "$WORK/out8" && find . | LC_ALL=C sort)" == $'.\n./lk\n./lk/real.txt' ]] \
   && grep -qx 'tar: lk/sym: not extracted (link)' <<< "$L_OUT" \
   && grep -qx 'tar: lk/hard: not extracted (link)' <<< "$L_OUT" \
   && grep -qx 'TARRC13=0x1' <<< "$L_OUT"; then
    pass "tar -x reports a symlink and a hardlink, extracts neither, exits 0x1"
else
    fail "tar -x mishandled link members" \
         "$L_OUT"$'\n'"$(cd "$WORK/out8" && find . | LC_ALL=C sort)"
fi

# ----------------------------------------------------------------------
# 14. A ':' left after sanitizing is refused.
# ----------------------------------------------------------------------
echo "=== colon: tar -x FS1:\\colon.tar -C FS1:\\out9 ==="
C_OUT="$(step_out 14)"
if [[ "$(cd "$WORK/out9" && find . | LC_ALL=C sort)" == $'.\n./ok\n./ok/fine.txt' ]] \
   && grep -qx 'tar: skipping unsafe member ok/fs1:/evil.txt' <<< "$C_OUT" \
   && grep -qx 'TARRC14=0x1' <<< "$C_OUT"; then
    pass "tar -x refuses a member whose sanitized name still holds ':'"
else
    fail "tar -x did not refuse a ':' member" \
         "$C_OUT"$'\n'"$(cd "$WORK/out9" && find . | LC_ALL=C sort)"
fi

# ----------------------------------------------------------------------
# 15. A host archive truncated inside a member, extracted over an existing
#     file: reported, the ORIGINAL left byte for byte, no temporary file
#     left beside it, exit 1.
# ----------------------------------------------------------------------
echo "=== truncated over an existing file: tar -x FS1:\\trunc.tar -C FS1:\\out10 ==="
T_OUT="$(step_out 15)"
if [[ "$(cd "$WORK/out10" 2>/dev/null && find . | LC_ALL=C sort)" == $'.\n./tr\n./tr/big.bin' ]] \
   && cmp -s "$WORK/out10/tr/big.bin" "$WORK/big.orig" \
   && grep -qx 'tar: FS1:\\out10\\tr\\big.bin: not extracted (member data incomplete or unreadable); any existing file is unchanged' <<< "$T_OUT" \
   && grep -qx 'tar: unexpected end of archive' <<< "$T_OUT" \
   && grep -qx 'TARRC15=0x1' <<< "$T_OUT"; then
    pass "tar -x leaves the existing file untouched by a truncated member, exits 0x1"
else
    fail "tar -x mishandled a truncated archive" \
         "$T_OUT"$'\n'"$(cd "$WORK/out10" 2>/dev/null && find . | LC_ALL=C sort)"
fi

# ----------------------------------------------------------------------
# 16-23. A host archive cut inside data, padding and a header: tar -t and
#        tar -x say "unexpected end of archive" and exit 1; cut at a header
#        boundary, both exit 0 with no such line. -x extracts every member
#        whose data was whole (a.txt, unless the cut is inside its data).
# ----------------------------------------------------------------------
check_cut() {   # <cut> <list step> <want error 0|1> <want a.txt extracted 0|1>
    local c="$1" t="$2" x=$(( $2 + 1 )) err="$3" ext="$4" ok=1 tout xout tree
    tout="$(step_out "$t")"
    xout="$(step_out "$x")"
    tree="$(cd "$WORK/xc$c" && find . | LC_ALL=C sort)"
    grep -qx 'a.txt' <<< "$tout" || ok=0
    grep -qx 'b.txt' <<< "$tout" && ok=0
    if [[ "$err" == 1 ]]; then
        grep -qx 'tar: unexpected end of archive' <<< "$tout" || ok=0
        grep -qx 'tar: unexpected end of archive' <<< "$xout" || ok=0
        grep -qx "TARRC$t=0x1" <<< "$tout" || ok=0
        grep -qx "TARRC$x=0x1" <<< "$xout" || ok=0
    else
        grep -q 'unexpected end' <<< "$tout$xout" && ok=0
        grep -qx "TARRC$t=0x0" <<< "$tout" || ok=0
        grep -qx "TARRC$x=0x0" <<< "$xout" || ok=0
    fi
    if [[ "$ext" == 1 ]]; then
        [[ "$tree" == $'.\n./a.txt' ]] && cmp -s "$WORK/xc$c/a.txt" "$WORK/csrc/a.txt" || ok=0
    else
        [[ "$tree" == "." ]] || ok=0
    fi
    if [[ "$ok" == 1 ]]; then
        pass "cut at $c: tar -t and -x $([[ $err == 1 ]] && echo 'report the truncation and exit 0x1' || echo 'read a clean end and exit 0x0')"
    else
        fail "cut at $c: tar -t / -x mishandled it" \
             "--- -t:"$'\n'"$tout"$'\n'"--- -x:"$'\n'"$xout"$'\n'"--- tree:"$'\n'"$tree"
    fi
}
echo "=== host archive cut at each position: tar -t / -x FS1:\\cutN.tar ==="
check_cut 700  16 1 0    # inside a.txt's data
check_cut 1520 18 1 1    # inside a.txt's padding: a.txt is whole
check_cut 1700 20 1 1    # inside b.txt's header
check_cut 1536 22 0 1    # at a header boundary: a clean end

# ----------------------------------------------------------------------
# 24. Members tar will not put in place: each refused with its reason in
#     words, the existing entries untouched, nothing stray, exit 1.
# ----------------------------------------------------------------------
echo "=== members that cannot be put in place: tar -x FS1:\\odd.tar -C FS1:\\outR ==="
O_OUT="$(step_out 24)"
O_TREE="$(cd "$WORK/outR" && find . | LC_ALL=C sort)"
o_ok=1
for want in \
    'tar: FS1:\outR\dirhere.txt: not extracted (a directory is in the way); any existing file is unchanged' \
    'tar: FS1:\outR\ro.txt: not extracted (the existing file or the volume is read-only); any existing file is unchanged' \
    'TARRC24=0x1'; do
    grep -qxF -- "$want" <<< "$O_OUT" || { o_ok=0; echo "  missing: $want"; }
done
[[ "$O_TREE" == $'.\n./dirhere.txt\n./ok.txt\n./ro.txt\n./~TAR12~1.TMP\n./~tar1234abcd.tmp' ]] || o_ok=0
[[ "$(cat "$WORK/outR/~TAR12~1.TMP")" == "~TAR12~1.TMP" ]] || o_ok=0
[[ "$(cat "$WORK/outR/~tar1234abcd.tmp")" == "~tar1234abcd.tmp" ]] || o_ok=0
[[ "$(cat "$WORK/outR/ro.txt")" == "keep" ]] || o_ok=0
[[ "$(cat "$WORK/outR/ok.txt")" == "ok.txt" ]] || o_ok=0
if [[ "$o_ok" == 1 ]]; then
    pass "tar -x names why a member cannot be put in place; names like tar's temporaries extract"
else
    fail "tar -x mishandled members it cannot put in place" \
         "$O_OUT"$'\n'"--- tree:"$'\n'"$O_TREE"
fi
chmod 0644 "$WORK/outR/ro.txt" 2>/dev/null || true

# tar's end-of-extract backstop never has a temporary left to remove, in
# any step: each failure path removes its own.
if grep -q 'tar: internal:' "$WORK/guest.clean"; then
    fail "tar's end-of-extract backstop found a temporary" \
         "$(grep 'tar: internal:' "$WORK/guest.clean")"
else
    pass "no temporary outlived its member in any step (backstop silent)"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
