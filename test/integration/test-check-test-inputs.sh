#!/bin/bash
# test-meta: arch=x64 needs= est=8 local-only=0
# test-check-test-inputs.sh -- scripts/check-test-inputs.py (gate.check-test-
# inputs) must FAIL on each way a tree file can reach a guest outside the
# skip-cache key, pass each way it is in it, and refuse to call a run it
# could not make "clean".
#
# Host-only; boots nothing. It runs the gate over a FIXTURE tree: copies of
# the key's own readers (lib/test-cache.sh, lib/build-products.sh,
# lib/discover.sh) under a private test/integration/, a build graph, and
# fixture tests -- one per case. Every verdict is compared EXACTLY: the
# gate's FAIL lines as a set, and its clean line word for word.
#
# Usage: ./test/integration/test-check-test-inputs.sh [--arch X64]

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
GATE="$PROJECT_DIR/scripts/check-test-inputs.py"
export TEST_SKIP_RATCHET=1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# fixture ROOT -- a source tree with the key's readers and a build graph in
# which fx-goal's product is fx.efi.
fixture() {
    local r="$1"
    mkdir -p "$r/test/integration/lib" "$r/share/dir" "$r/build/x64-debug" "$r/scripts"
    cp "$SCRIPT_DIR/lib/test-cache.sh" "$SCRIPT_DIR/lib/build-products.sh" \
       "$SCRIPT_DIR/lib/discover.sh" "$r/test/integration/lib/"
    printf 'x\n' > "$r/share/a.json5"
    printf 'x\n' > "$r/share/dir/in.json5"
    cat > "$r/build/x64-debug/build.ninja" <<'NINJA'
rule cp
  command = cp $in $out
build fx.efi: cp src.c
build fx-goal: phony fx.efi
NINJA
}
# ft ROOT NAME BODY -- a fixture test (a test-meta header, then BODY)
ft() { printf '#!/bin/bash\n# test-meta: arch=x64 est=1%s\n%s\n' "${4:-}" "$3" > "$1/test/integration/$2"; }

run_gate() {   # run_gate ROOT -> $OUT (stdout+stderr), $RC
    OUT=$(python3 "$GATE" --root "$1" --tree "$1/build/x64-debug" 2>&1)
    RC=$?
}

echo "=== check-test-inputs: what reaches the guest must reach the key ==="

# --- the cases a key covers: CLEAN, and counted exactly ----------------------
G="$WORK/good"; fixture "$G"
ft "$G" test-lit.sh 'cp "$PROJECT_DIR/share/a.json5" "$TEST_STAGING/"'
ft "$G" test-var.sh 'F="${PROJECT_DIR}/share/a.json5"
cp "$F" "$TEST_STAGING/x.json5"'
ft "$G" test-bld.sh 'cp "$(test_build_dir)/fx.efi" "$TEST_STAGING/"' ' builds=fx-goal'
ft "$G" test-rec.sh 'cp "$(test_build_dir)/other.efi" "$TEST_STAGING/"
test_record_input "$(test_build_dir)/other.efi"'
ft "$G" test-mark.sh '# test-input: not-keyed -- a corpus, fetched on purpose
launcher --mount "$PROJECT_DIR/share/dir"'
ft "$G" test-mntok.sh 'V="$(mktemp -d)"
cp "$PROJECT_DIR/share/dir/in.json5" "$V/"
launcher --mount "$V"'
# Not code, so not judged: a heredoc and a multi-line string holding a
# fixture script, and a comment.
ft "$G" test-text.sh 'cat > "$TMP/stub.sh" <<EOF
cp "$PROJECT_DIR/share/$n" "$TEST_STAGING/"
EOF
BODY='"'"'x
cp "$PROJECT_DIR/share/$n" "$TEST_STAGING/"
'"'"'
# cp "$PROJECT_DIR/share/$n" "$TEST_STAGING/"'
ft "$G" test-temp.sh 'printf "x\n" > "$TEST_STAGING/gen.txt"'
# What it cannot resolve -- a function argument, a run-time value -- is not
# judged, but COUNTED: blindness shows in the clean line.
ft "$G" test-blind.sh 'stage() { cp "$1" "$TEST_STAGING/"; }
R=$(cd "$PROJECT_DIR/share" && pwd)
cp "$R/a.json5" "$TEST_STAGING/"'
run_gate "$G"
want="check-test-inputs: clean -- 10 staged source(s) in 9 tests and 3 libs: 3 keyed by literal path, 1 by builds=, 1 recorded, 0 harness, 1 marked not-keyed, 2 not tree files; 2 unresolved and 0 unparsed line(s) not judged."
if [[ $RC -eq 0 && "$OUT" == "$want" ]]; then
    pass "every covered input is judged covered, and counted (the control: the gate can pass)"
else
    fail "the covered fixture: rc=$RC, got [$OUT]"
fi

# --- each way an input escapes the key: FAIL, by name --------------------------
B="$WORK/bad"; fixture "$B"
ft "$B" test-ok.sh 'cp "$PROJECT_DIR/share/a.json5" "$TEST_STAGING/"'
ft "$B" test-dir.sh 'cp -r "$PROJECT_DIR/share/dir" "$TEST_STAGING/"'
ft "$B" test-expand.sh 'sed s/a/b/ "$PROJECT_DIR/share/$n.conf" > "$TEST_STAGING/x.conf"'
ft "$B" test-mount.sh 'launcher --mount "$PROJECT_DIR/share/dir"'
ft "$B" test-unbuilt.sh 'cp "$(test_build_dir)/other.efi" "$TEST_STAGING/"' ' builds=fx-goal'
ft "$B" test-mntdir.sh 'V="$(mktemp -d)"
cp "$PROJECT_DIR/share/$n" "$V/"
launcher --mount "$V"'
ft "$B" test-tee.sh 'cat "$PROJECT_DIR/share/$n" | tee "$TEST_STAGING/t" >/dev/null'
# A copy behind a keyword or a command prefix is still a copy; --mount=PATH
# is a mount; dd's if= is a source.
ft "$B" test-then.sh 'if true; then cp "$PROJECT_DIR/share/$a" "$TEST_STAGING/"; fi'
ft "$B" test-prefix.sh 'timeout 5 cp "$PROJECT_DIR/share/$b" "$TEST_STAGING/"'
ft "$B" test-prefix2.sh 'timeout -k 5 10 cp "$PROJECT_DIR/share/$d" "$TEST_STAGING/"
flock -x "$L" cp "$PROJECT_DIR/share/$e" "$TEST_STAGING/"
env -u FOO cp "$PROJECT_DIR/share/$f" "$TEST_STAGING/"'
ft "$B" test-mnteq.sh 'launcher --mount="$PROJECT_DIR/share/dir"'
ft "$B" test-dd.sh 'dd if="$PROJECT_DIR/share/$c" of="$TEST_STAGING/img" bs=1M'
# A file name that is an expansion alone names ANY product: not proven one.
ft "$B" test-anyprod.sh 'cp "$(test_build_dir)/$name" "$TEST_STAGING/"' ' builds=fx-goal'
# A bare relative path to a tree file (after a cd) is no literal the key reads.
ft "$B" test-rel.sh 'cd "$PROJECT_DIR" || exit 1
cp share/a.json5 "$TEST_STAGING/"'
# The marker is a COMMENT with a reason: in a string, or with none, it is not.
ft "$B" test-badmark.sh 'echo "# test-input: not-keyed -- in a string"; launcher --mount "$PROJECT_DIR/share/dir"
# test-input: not-keyed
launcher --mount "$PROJECT_DIR/share/dir"'
# A variable a LIB sets resolves a test's word (TEST_BUILD_DIR, as the real
# libs set it) -- and a lib's own `TEST_STAGING=$(mktemp ...)` must not hide
# that a write goes into the staging.
ft "$B" test-libvar.sh 'cp "$FXDIR/other.efi" "$TEST_STAGING/"' ' builds=fx-goal'
ft "$B" test-stagevar.sh 'D="$TEST_STAGING/drv"
cp "$PROJECT_DIR/share/$n" "$D/"'
printf 'TEST_STAGING="$(mktemp -d)"\nFXDIR="$(test_build_dir)"\nhelper() {\n    cp "$(test_build_dir)/fx.efi" "$TEST_STAGING/"\n}\n' \
    > "$B/test/integration/lib/fx-lib.sh"
run_gate "$B"
got=$(grep '^    test/' <<< "$OUT" | LC_ALL=C sort)
want=$(LC_ALL=C sort <<'EOF'
    test/integration/test-dir.sh:3: a copy into the guest of $PROJECT_DIR/share/dir -- not a literal path to a file the key reads
    test/integration/test-expand.sh:3: a copy into the guest of $PROJECT_DIR/share/$n.conf -- not a literal path to a file the key reads
    test/integration/test-mount.sh:3: a --mount of $PROJECT_DIR/share/dir -- a directory is never keyed
    test/integration/test-unbuilt.sh:3: a copy into the guest of $(test_build_dir)/other.efi -- not a product of this test's builds=
    test/integration/test-mntdir.sh:4: a copy into the guest of $PROJECT_DIR/share/$n -- not a literal path to a file the key reads
    test/integration/test-tee.sh:3: a copy into the guest of $PROJECT_DIR/share/$n -- not a literal path to a file the key reads
    test/integration/test-then.sh:3: a copy into the guest of $PROJECT_DIR/share/$a -- not a literal path to a file the key reads
    test/integration/test-prefix.sh:3: a copy into the guest of $PROJECT_DIR/share/$b -- not a literal path to a file the key reads
    test/integration/test-prefix2.sh:3: a copy into the guest of $PROJECT_DIR/share/$d -- not a literal path to a file the key reads
    test/integration/test-prefix2.sh:4: a copy into the guest of $PROJECT_DIR/share/$e -- not a literal path to a file the key reads
    test/integration/test-prefix2.sh:5: a copy into the guest of $PROJECT_DIR/share/$f -- not a literal path to a file the key reads
    test/integration/test-mnteq.sh:3: a --mount of $PROJECT_DIR/share/dir -- a directory is never keyed
    test/integration/test-dd.sh:3: a copy into the guest of $PROJECT_DIR/share/$c -- not a literal path to a file the key reads
    test/integration/test-anyprod.sh:3: a copy into the guest of $(test_build_dir)/$name -- not a product of this test's builds=
    test/integration/test-rel.sh:4: a copy into the guest of share/a.json5 -- not a literal path to a file the key reads
    test/integration/test-badmark.sh:3: a --mount of $PROJECT_DIR/share/dir -- a directory is never keyed
    test/integration/test-badmark.sh:5: a --mount of $PROJECT_DIR/share/dir -- a directory is never keyed
    test/integration/test-libvar.sh:3: a copy into the guest of $FXDIR/other.efi -- not a product of this test's builds=
    test/integration/test-stagevar.sh:4: a copy into the guest of $PROJECT_DIR/share/$n -- not a literal path to a file the key reads
    test/integration/lib/fx-lib.sh:4: a copy into the guest of $(test_build_dir)/fx.efi -- a build output a lib stages without recording it
EOF
)
if [[ $RC -eq 1 && "$got" == "$want" ]]; then
    pass "each unkeyed input FAILS the gate, named file:line, and nothing else does"
else
    fail "the unkeyed fixture: rc=$RC, got:"; printf '%s\n' "$OUT" | sed 's/^/      /'
fi
if grep -q '^      # test-input: not-keyed -- <why>$' <<< "$OUT"; then
    pass "a FAIL names the marker that would justify the input"
else fail "a FAIL does not name the marker"; fi

# --- what it could not see is no answer -------------------------------------
N="$WORK/blind"; fixture "$N"
ft "$N" test-nothing.sh 'echo hi'
run_gate "$N"
if [[ $RC -eq 1 && "$OUT" == "check-test-inputs: FAIL -- found no staging write at all in 1 tests and 3 libs. The parser is blind; a gate that"* ]]; then
    pass "a tree with no staging write at all FAILS (a gate that sees nothing is not clean)"
else fail "no staging write: rc=$RC, got [$OUT]"; fi
rm "$G/build/x64-debug/build.ninja"
run_gate "$G"
if [[ $RC -eq 1 && "$OUT" == "check-test-inputs: FAIL -- nothing to check: 9 test(s) under $G/test/integration, no build.ninja in $G/build/x64-debug" ]]; then
    pass "an unconfigured tree FAILS, said (it cannot know the builds= products)"
else fail "no build.ninja: rc=$RC, got [$OUT]"; fi
fixture "$G"
printf 'exit 3\n' > "$G/test/integration/lib/test-cache.sh"
run_gate "$G"
if [[ $RC -eq 1 && "$OUT" == "check-test-inputs: FAIL -- $G/test/integration/lib/test-cache.sh --literals could not answer (exit 3):" ]]; then
    pass "a key reader that cannot answer FAILS the gate (no answer is not \"nothing keyed\")"
else fail "a broken reader: rc=$RC, got [$OUT]"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]]
