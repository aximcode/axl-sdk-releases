#!/bin/bash
# test-meta: arch=X64 needs=gpu est=40 local-only=1
# test-kbtune-qemu.sh — smoke test for the kbtune GOP UI (tools/kbtune.efi).
#
# kbtune is a full-screen GOP tool, so there is no text output to scrape; the
# check is that it comes up on a GOP framebuffer, attaches keyboard input,
# processes injected keys without crashing, and renders a non-trivial HUD.
# Launch under --gpu, inject a few keystrokes + a mode switch (F2) via the QEMU
# monitor, and capture a screenshot: a rendered HUD is a sizeable PNG, a blank
# / crashed screen is tiny. Real-HW verification over the KVM is the definitive
# test (see docs/AXL-KbTune-Design.md). LOCAL-ONLY: needs a GPU-capable QEMU.
#
# Usage: ./test/integration/test-kbtune-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

TEST_ARCH="${TEST_ARCH:-X64}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
_arch_lc=$(echo "$TEST_ARCH" | tr 'A-Z' 'a-z'); [[ "$_arch_lc" == "aarch64" ]] && _arch_lc="aa64"
KBTUNE="$PROJECT_DIR/out/native-$_arch_lc/tools/kbtune.efi"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

make -C "$PROJECT_DIR" ARCH="$_arch_lc" tools >/dev/null 2>&1
if [[ ! -f "$KBTUNE" ]]; then
    echo "WARN: kbtune not built at $KBTUNE; skipping."
    echo "test-kbtune: SKIP"; exit 0
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
NSH="$WORK/kbt.nsh"; printf '@echo -off\nkbtune.efi\n' > "$NSH"
SHOT="$WORK/kbtune.png"

# Inject: type some chars, switch to axEdit (F2), type more, then Esc to quit.
timeout 90 "$RUN_QEMU" --arch "$TEST_ARCH" --nsh "$NSH" \
    --extra "$KBTUNE:kbtune.efi" \
    --screenshot "$SHOT" --sendkey "a b c f2 x y z esc" \
    --timeout 30 "$KBTUNE" >"$WORK/run.log" 2>&1 || true

pass=0; fail=0
check() { if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1)); else echo "FAIL: $2"; fail=$((fail+1)); fi; }

# `[[ cond ]]; check "$?"` is unsafe here: common-test.sh runs under `set -e`
# and this script does NOT `set +e` (unlike the axl-cc/install tests, which do),
# so a FAILING assertion would kill the run before its own FAIL line, taking
# every later check and the summary with it. An assertion that cannot report
# its own failure is worse than no assertion.
_ck=0; [[ -f "$SHOT" ]] || _ck=1
check "$_ck" "kbtune produced a GOP screenshot"
# A rendered HUD PNG is several KB; a blank/crashed framebuffer compresses tiny.
sz=$(stat -c %s "$SHOT" 2>/dev/null || echo 0)
_ck=0; [[ "$sz" -gt 4096 ]] || _ck=1
check "$_ck" "screenshot is a non-blank HUD (${sz} bytes > 4096)"

echo "--- results ---"
echo "kbtune GOP smoke: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
