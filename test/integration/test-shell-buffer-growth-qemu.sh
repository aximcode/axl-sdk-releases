#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0 builds=shell-growth-test guest=none
# guest=none: a second disk (TEST_DISK2) whose FAT it reads back with
#   mtools: a machine no class gives.
# test-shell-buffer-growth-qemu.sh -- path operations under the modern Shell
# leave O(1) parked buffers, not O(N).
#
# THE DEFECT. EFI_SHELL_PROTOCOL parks the buffer several calls return on the
# Shell's deferred-free list (ShellInfoObject.BufferToFreeList), emptied only
# when the CURRENT SHELL COMMAND ends: GetCurDir, GetEnv, GetMapFromDevicePath,
# and through them GetDevicePathFromFilePath on a relative path and
# OpenFileByName on every open (OpenRootByHandle names the root through
# GetMapFromDevicePath). An app looping for hours, or a resident driver whose
# calls park onto whatever command is current while the Shell idles at its
# prompt, grows without bound.
#
# One fixture run is ONE shell command. It samples the firmware's free pages
# around N operations of each class and prints `SHGROW <class> <kind> n=N
# ok=K pages=P`. The CONTROL is the raw Shell OpenFileByName: it must show
# growth, or a small figure elsewhere is the sampler's blindness.
#
#   RED    SHGROW read abs n=400 ok=400 pages=25     (rel 50; mkdir rel 175)
#   GREEN  SHGROW read abs n=400 ok=400 pages=0      (every class, both arches)
#
# THE BOUND. One parked buffer per call measured exactly 25 pages per 400
# calls on both arches (a BUFFER_LIST node plus the buffer, pool-allocated):
# one page per 16 calls. A class that still parked once per call would read
# >= N/16. The bound is N/200 pages (2 at N=400): an eighth of that, which no
# per-call parking can meet, and room for a pool page moving on its own. Each
# class is warmed up with N/8 of the same operation before it is sampled, so
# one-off growth (a cache's first lookup, AxlMem's DEBUG free quarantine --
# up to 64 KiB of freed blocks -- taking on the class's block sizes, measured
# 5-7 pages unwarmed whatever N was) lands outside the sample.
#
# THE OUTCOMES. Resolving paths AXL-side must answer exactly what the Shell
# answered. `func` prints the outcome of every tricky case -- case and
# separators, `.`/`..`, a one-character name, an 8.3 alias, relative to the
# current directory, missing file vs directory vs volume vs map name, stat of
# the root in every spelling, canonical names -- and the current directory and
# a volume's names across every way they change under the image: its own
# chdir, the Shell's Execute (a nested shell), another image started with
# StartImage, and its own SetMap/unmap down to an empty map. The expected
# lines are the outcomes the Shell itself produced: recorded from the code
# before the change, on both arches, and re-checked against it by reverting
# the change under scripts/sabotage.sh (every outcome check passed there, and
# only the growth checks failed).
#
# Each check was sabotaged and seen to fail here: the cwd reused without the
# `cwd` variable matching (chdir, child-image and converge checks) or
# without its volume still mapped (empty-map check); the fs<n> probe taking
# several matches as one (outcomes) or matching another volume's name (the
# enumerate bound and unmap check -- x64, which has a second volume); and
# the Shell's one-character-remainder quirk dropped. Not detectable here:
# refusing to keep a cwd the variable does not vouch for, because the EDK2
# Shell never lets the two differ.
#
# SHGROW_N=<n> changes N; SHGROW_DIAG=1 prints the `cwd` variable beside
# every cwd answer.
#
# Usage: ./test/integration/test-shell-buffer-growth-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$(test_build_dir)"
N=${SHGROW_N:-400}

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" \
    shell-growth-test 2>&1 | tail -1

pass=0
fail=0
check() {  # check <msg> <cmd...>
    local msg="$1"; shift
    if "$@"; then
        echo "  PASS: $msg"; pass=$((pass + 1))
    else
        echo "  FAIL: $msg"; fail=$((fail + 1))
    fi
}

echo "=== Shell Buffer Growth Test ($TEST_ARCH) ==="

test_add_efi "$NATIVE_DIR/shell-growth-test.efi"
printf 'HELLO\n'  > "$TEST_STAGING/dof_in.txt"
printf 'A\n'      > "$TEST_STAGING/a"
printf 'LONG\n'   > "$TEST_STAGING/LongFileName.txt"
mkdir -p "$TEST_STAGING/sub"
printf 'NESTED\n' > "$TEST_STAGING/sub/nested.txt"
mkdir -p "$TEST_STAGING/work"
: > "$TEST_STAGING/work/keep.txt"
# A second volume (fs1:, on USB): a volume's fs<n> name is found by probing
# FS0..FS63 for the ones that map to IT, which only means something when
# another volume's name maps elsewhere. x64 only: on aa64 the Shell maps the
# USB disk ahead of the boot disk, which would make it fs0:.
if [[ "$TEST_ARCH" == X64 ]]; then
    export TEST_DISK2="$TEST_TMPDIR/disk2.img"
    dd if=/dev/zero of="$TEST_DISK2" bs=1M count=4 status=none
    mkfs.vfat -n TWO "$TEST_DISK2" > /dev/null
fi

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "shell-growth-test.efi growth $N"
    echo "shell-growth-test.efi func"
    echo "echo SHGROW_EXIT"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 120
test_clean_log

echo "--- serial log (SHGROW lines) ---"
{ grep -a '^SHGROW' "$TEST_CLEAN_LOG" || true; } | sed 's/^/  /'
echo "--- assertions ---"

# grow_field <class> <kind> <field>
grow_field() {
    grep -a "^SHGROW $1 $2 n=$N " "$TEST_CLEAN_LOG" | head -1 \
        | sed -n "s/.* $3=\\(-\\{0,1\\}[0-9]*\\).*/\\1/p"
}

# The control first: the raw Shell open must read as parked growth -- one
# page per 16 opens relative or absolute (25 per 400 absolute, 50 relative,
# measured on both arches) -- or every zero below is the sampler's blindness.
expect_control() {
    local kind="$1" pages ok
    pages=$(grow_field control "$kind" pages)
    ok=$(grow_field control "$kind" ok)
    [[ -n "$pages" && "$ok" == "$N" ]] && (( pages >= N / 40 ))
}
check "control: raw Shell OpenFileByName (relative) parks visibly (>= $((N / 40)) pages)" \
    expect_control rel
check "control: raw Shell OpenFileByName (absolute) parks visibly (>= $((N / 40)) pages)" \
    expect_control abs

# expect_flat <class> <kind> -- every operation succeeded, and the class
# parked O(1): at most N/200 pages over N operations.
expect_flat() {
    local pages ok
    pages=$(grow_field "$1" "$2" pages)
    ok=$(grow_field "$1" "$2" ok)
    [[ -n "$pages" && "$ok" == "$N" ]] && (( pages <= N / 200 ))
}
for cls in read write stat delete mkdir list canon; do
    for kind in rel abs; do
        check "$cls ($kind): $N operations park O(1) (<= $((N / 200)) pages)" \
            expect_flat "$cls" "$kind"
    done
done
check "volume enumerate: $N calls park O(1) (<= $((N / 200)) pages)" expect_flat enum -
check "getcwd: $N calls park O(1) (<= $((N / 200)) pages)" expect_flat getcwd -
check "the growth run finished" grep -aq '^SHGROW done' "$TEST_CLEAN_LOG"

# ---------------------------------------------------------------------------
# The outcomes. Recorded from the Shell itself (the code before the change,
# both arches): every line, in order, byte for byte. @CONSIST@ is the Shell's
# consistent name for the boot disk, which differs by arch (the disk sits on
# a different bus); every other line is the same on both.
# ---------------------------------------------------------------------------
declare -A _CONSIST=([X64]=F0a65535a [AARCH64]=F0)
CONSIST="${_CONSIST[$TEST_ARCH]}"
cat > "$TEST_TMPDIR/shfunc.want" <<'EOF'
SHFUNC abs rc=0 HELLO|
SHFUNC abs-upper rc=0 HELLO|
SHFUNC abs-mixed-nosep rc=0 HELLO|
SHFUNC abs-slash rc=0 NESTED|
SHFUNC abs-dotdot rc=0 HELLO|
SHFUNC abs-dot rc=0 HELLO|
SHFUNC abs-trailing-sep rc=-1
SHFUNC abs-onechar rc=0 A|
SHFUNC abs-onechar-nosep rc=0
SHFUNC abs-8.3 rc=0 LONG|
SHFUNC abs-long-case rc=0 LONG|
SHFUNC rel rc=0 HELLO|
SHFUNC rel-sub rc=0 NESTED|
SHFUNC rel-dot rc=0 HELLO|
SHFUNC rel-rootsep rc=0 NESTED|
SHFUNC rel-onechar rc=0 A|
SHFUNC miss-file-abs rc=-1
SHFUNC miss-file-rel rc=-1
SHFUNC miss-dir-abs rc=-1
SHFUNC miss-dir-rel rc=-1
SHFUNC miss-vol rc=-1
SHFUNC miss-map rc=-1
SHFUNC miss-map-nosep rc=-1
SHFUNC miss-delete-abs rc=-5
SHFUNC miss-delete-rel rc=-5
SHFUNC miss-delete-vol rc=-1
SHFUNC miss-delete-map rc=-1
SHFUNC info-root-bare rc=-1 dir=-1 size=0
SHFUNC info-root rc=0 dir=1 size=0
SHFUNC info-dir rc=0 dir=1 size=512
SHFUNC info-dir-sep rc=0 dir=1 size=512
SHFUNC info-dir-rel rc=0 dir=1 size=512
SHFUNC info-file-rel rc=0 dir=0 size=7
SHFUNC info-miss-vol rc=-1 dir=-1 size=0
SHFUNC isdir-rel 1
SHFUNC isdir-abs 1
SHFUNC canon-8.3 rc=0 [LongFileName.txt]
SHFUNC canon-case rc=0 [sub\nested.txt]
SHFUNC canon-rel-dotdot rc=0 [dof_in.txt]
SHFUNC canon-rel-missing rc=0 [sub\missing.txt]
SHFUNC canon-missing-dir rc=-5 []
SHFUNC canon-miss-vol rc=-1 []
SHFUNC canon-root rc=0 []
SHFUNC mkdir-rel rc=0
SHFUNC mkdir-rel-info rc=0 dir=1 size=512
SHFUNC rmdir-abs rc=0
SHFUNC write-rel rc=0
SHFUNC write-rel-read rc=0 M|
SHFUNC delete-rel rc=0
SHFUNC delete-rel-gone rc=-1
SHFUNC cwd-start cwd=[FS0:]
SHFUNC cwd-start-rel rc=0 HELLO|
SHFUNC chdir-sub rc=0
SHFUNC cwd-after-chdir cwd=[FS0:sub]
SHFUNC chdir-rel rc=0 NESTED|
SHFUNC chdir-rel-dotdot rc=0 HELLO|
SHFUNC chdir-rel-rootsep rc=0 HELLO|
SHFUNC chdir-canon rc=0 [sub\nested.txt]
SHFUNC chdir-up rc=0
SHFUNC cwd-after-up cwd=[FS0:..]
SHFUNC up-rel rc=-1
SHFUNC chdir-root rc=0
SHFUNC cwd-after-root cwd=[FS0:]
SHFUNC root-rel rc=0 HELLO|
SHFUNC exec-cd rc=0
SHFUNC cwd-after-exec cwd=[FS0:]
SHFUNC exec-rel rc=0 HELLO|
SHFUNC exec-rel-sub rc=-1
SHFUNC pre-converge-cwd cwd=[FS0:]
SHFUNC converge rc=0
SHFUNC converged-cwd cwd=[FS0:\sub]
SHFUNC converged-rel rc=0 NESTED|
SHFUNC converge-back rc=0
SHFUNC converged-back-cwd cwd=[FS0:]
SHFUNC child-cd rc=0
SHFUNC cwd-after-child cwd=[FS0:sub]
SHFUNC child-rel rc=0 NESTED|
SHFUNC child-cd-back rc=0
SHFUNC cwd-after-child-back cwd=[FS0:]
SHFUNC child-back-rel rc=0 HELLO|
SHFUNC aba1-chdir rc=0
SHFUNC aba1-child-exec rc=0
SHFUNC aba1-mid-cwd cwd=[FS0:\sub]
SHFUNC aba1-child-cd rc=0
SHFUNC aba1-child-exec2 rc=0
SHFUNC aba1-cwd cwd=[FS0:]
SHFUNC aba1-rel rc=0 HELLO|
SHFUNC aba1-back rc=0
SHFUNC aba2-chdir rc=0
SHFUNC aba2-start-cwd cwd=[FS0:\sub]
SHFUNC aba2-child-cd rc=0
SHFUNC aba2-exec rc=0
SHFUNC aba2-cwd cwd=[FS0:]
SHFUNC aba2-rel rc=0 HELLO|
SHFUNC aba2-back rc=0
SHFUNC aba3-chdir rc=0
SHFUNC aba3-start-cwd cwd=[FS0:\sub]
SHFUNC aba3-chdir-root rc=0
SHFUNC aba3-child-exec rc=0
SHFUNC aba3-cwd cwd=[FS0:]
SHFUNC aba3-rel rc=0 HELLO|
SHFUNC aba3-back rc=0
SHFUNC aba-end-cwd cwd=[FS0:]
SHFUNC fs0-dp found
SHFUNC vols-start rc=0 @VOLS@
SHFUNC names-start name rc=0 [fs0] alias rc=0 [FS0]
SHFUNC setmap-qq rc=0
SHFUNC names-after-setmap name rc=0 [fs0] alias rc=0 [FS0]
SHFUNC setmap-fs7 rc=0
SHFUNC reset-fs0 rc=0
SHFUNC names-after-reorder name rc=0 [fs7] alias rc=0 [@CONSIST@]
SHFUNC unmap-fs7 rc=0
SHFUNC names-after-unmap-fs7 name rc=0 [fs0] alias rc=0 [@CONSIST@]
SHFUNC child-setmap-fs7 rc=0
SHFUNC names-after-child-fs7 name rc=0 [fs0] alias rc=0 [@CONSIST@]
SHFUNC child-reset-fs0 rc=0
SHFUNC names-after-child-reorder name rc=0 [fs7] alias rc=0 [@CONSIST@]
SHFUNC child-unmap-fs7 rc=0
SHFUNC names-after-child-unmap-fs7 name rc=0 [fs0] alias rc=0 [@CONSIST@]
SHFUNC vols-after-child-unmap-fs7 rc=0 @VOLS@
SHFUNC alias-read rc=0 HELLO|
SHFUNC alias-read-upper-nosep rc=0 HELLO|
SHFUNC unmap-fs0 rc=0
SHFUNC names-after-unmap-fs0 name rc=-1 [] alias rc=0 [@CONSIST@]
SHFUNC unmapped-read rc=-1
SHFUNC unmapped-alias-read rc=0 HELLO|
SHFUNC unmapped-cwd cwd=[FS0:]
SHFUNC unmapped-rel rc=-1
SHFUNC unmapped-delete-rel rc=-5
SHFUNC unmap-qq rc=0
SHFUNC names-after-unmap-qq name rc=-1 [] alias rc=0 [@CONSIST@]
SHFUNC remap-fs0 rc=0
SHFUNC names-after-remap name rc=0 [fs0] alias rc=0 [@CONSIST@]
SHFUNC remap-read rc=0 HELLO|
SHFUNC remap-cwd cwd=[FS0:]
SHFUNC remap-rel rc=0 HELLO|
SHFUNC unmap-all failures=0
SHFUNC names-all-unmapped name rc=-1 [] alias rc=-1 []
SHFUNC all-unmapped-cwd cwd=[(null)]
SHFUNC all-unmapped-rel rc=-1
SHFUNC all-unmapped-delete-rel rc=-1
SHFUNC restore-fs0 rc=0
SHFUNC restored-cwd cwd=[FS0:]
SHFUNC restored-rel rc=0 HELLO|
SHFUNC done
EOF
sed -i "s/@CONSIST@/$CONSIST/" "$TEST_TMPDIR/shfunc.want"
# The volumes, in enumeration order: x64 has the second (USB) disk.
declare -A _VOLS=([X64]='count=2 [fs1 0 fs1] [fs0 0 fs0]' [AARCH64]='count=1 [fs0 0 fs0]')
VOLS="${_VOLS[$TEST_ARCH]}"
sed -i "s/@VOLS@/$VOLS/" "$TEST_TMPDIR/shfunc.want"
{ grep -a '^SHFUNC' "$TEST_CLEAN_LOG" || true; } | tr -d '\r' | sed 's/ *$//' \
    > "$TEST_TMPDIR/shfunc.got"

# outcome <case> -- the case's line is exactly the Shell's.
outcome() {
    local want
    want=$(grep -m1 "^SHFUNC $1 " "$TEST_TMPDIR/shfunc.want")
    [[ -n "$want" ]] && grep -qxF -- "$want" "$TEST_TMPDIR/shfunc.got"
}
echo "--- outcomes vs the Shell's ---"
if ! diff "$TEST_TMPDIR/shfunc.want" "$TEST_TMPDIR/shfunc.got" > "$TEST_TMPDIR/shfunc.diff" \
   || [[ -n "${SHGROW_DIAG:-}" ]]; then
    sed 's/^/  /' "$TEST_TMPDIR/shfunc.diff"
    # The Shell's `cwd` variable beside each cwd answer: what a failure in
    # the cached current directory is read against. Diagnostic only.
    { grep -a '^SHDIAG' "$TEST_CLEAN_LOG" || true; } | tr -d '\r' | sed 's/^/  /'
fi
check "every outcome is the Shell's, in order ($(wc -l < "$TEST_TMPDIR/shfunc.want") lines)" \
    cmp -s "$TEST_TMPDIR/shfunc.want" "$TEST_TMPDIR/shfunc.got"
# Named, so a failure says WHICH change went unseen. Each is preceded in the
# fixture by a relative operation, so the old directory or names were in hand.
check "this image's chdir is seen (cwd and a relative read)" \
    eval 'outcome cwd-after-chdir && outcome chdir-rel'
check "a chdir back to the root is seen" \
    eval 'outcome cwd-after-root && outcome root-rel'
check "the Shell's Execute (a nested shell's cd) leaves the cwd, as the Shell does" \
    eval 'outcome cwd-after-exec && outcome exec-rel && outcome exec-rel-sub'
check "a chdir by another image (StartImage) is seen" \
    eval 'outcome cwd-after-child && outcome child-rel && outcome cwd-after-child-back'
check "a chdir to where a nested shell's cd went is seen" \
    eval 'outcome converged-cwd && outcome converged-rel'
check "a cwd moved away and back by nested shells and other images is the shell's" \
    eval 'outcome aba1-cwd && outcome aba1-rel && outcome aba2-cwd && outcome aba2-rel && outcome aba3-cwd && outcome aba3-rel'
check "a volume whose first name is unmapped is renamed (names after unmap)" \
    outcome names-after-unmap-fs0
check "a name set again moves to the end, as with the Shell (names after reorder)" \
    eval 'outcome names-after-reorder && outcome names-after-unmap-fs7'
check "...and when ANOTHER image sets it again (names after child reorder)" \
    eval 'outcome names-after-child-reorder && outcome names-after-child-unmap-fs7'
check "a volume given back its fs name is seen (names after remap)" \
    outcome names-after-remap
check "a cwd whose volume is unmapped resolves nothing, as with the Shell" \
    eval 'outcome unmapped-rel && outcome unmapped-delete-rel && outcome remap-rel'
check "with every name unmapped there is no cwd (the cwd variable survives it)" \
    eval 'outcome all-unmapped-cwd && outcome all-unmapped-delete-rel && outcome restored-cwd'

check "the shell survived every case" grep -aq '^SHGROW_EXIT' "$TEST_CLEAN_LOG"

echo ""
echo "shell-buffer-growth ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
