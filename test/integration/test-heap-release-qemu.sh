#!/bin/bash
# test-meta: arch=both needs= est=35 local-only=0 builds=AXL_CPP=1,heap-release-test,cxx-ctor-test guest=standard
# test-heap-release-qemu.sh — an image gives its C heap back when it goes.
#
# THE DEFECT. newlib's dlmalloc grows into a region _sbrk takes with
# AllocatePages (src/cxxrt/axl-cxxrt-alloc.c), and nothing ever returned it.
# UEFI does not reclaim an image's page allocations at unload, so every launch
# of an image that had called malloc left its arena -- 256 pages, 1 MiB --
# behind. That is every C++ image, not only ones that allocate: libsupc++'s
# emergency-pool constructor mallocs ~1.1 KiB before main, and on a
# --no-eh-frame image (where that pool can never be used) it is the ONLY
# thing that does. Introduced by 442c442f (the two-allocator split), shipped
# from v4.2.0.
#
#   RED    HEAPREL heap-rel-cxx-noeh.efi launches=5 per_launch=256 total=1280
#   GREEN  HEAPREL heap-rel-cxx-noeh.efi launches=5 per_launch=0 total=0
#
# One observer (heap-release-test.efi, plain C) launches each child N times and
# counts the firmware's free pages around them. The leak control is the case
# that makes the zeros mean something: a child that keeps 64 pages must read
# as exactly 64 per launch.
#
# THE DRIVER CASE IS A DIFFERENCE, not a zero. A driver load/unload cycle
# reads ~0.4 pages even when the image never starts: Shell buffers held until
# the current shell command ends, not a leak (see heap-release-test.c) -- so
# the driver is run
# `load` (never started) and `driver` (started, then unloaded) over the same
# N, and what STARTING it costs, where its heap lives, must be ~0. Before the
# fix that difference was 2560 pages over 10 cycles.
#
# ORDER. The release must come after everything that can still use the heap.
# heap-rel-order.efi's global destructor allocates 3 MiB during teardown --
# more than the 1 MiB first commit, so it must GROW the heap, which _sbrk
# refuses once the heap is released -- and prints `HEAPORDER ok` or
# `refused`. The growth also makes its arena multi-piece, so its zero covers
# releasing an extended arena too.
#
# A RESIDENT driver keeps its heap. A driver that calls axl_exit(0) in its
# entry runs _axl_cleanup and then stays resident (Exit with EFI_SUCCESS);
# releasing there would leave a live driver allocating from freed pages.
# `hold` measures it while resident (must still hold the arena) and after
# unload (must have given it back).
#
# A DEBUG driver (the in-tree cxx-ctor-driver.efi, AXL_MEM_DEBUG) must also
# give back AxlMem's free quarantine on unload -- up to 64 KiB of freed pool
# the ring holds for use-after-free checks, which only the leak report used to
# drain, and a plain driver's unload never ran it: ~2.9 pages per cycle.
#
# Every other C++ image is linked by the staged axl-c++: --no-eh-frame has no
# in-tree build rule, and a RELEASE driver is what a consumer ships (an in-tree
# DEBUG one also holds AxlMem's free quarantine, which is not this defect).
# Those images link the STAGED libaxl.a and objects, which a src/ edit does
# NOT refresh. run-integration.sh restages before it runs anything (unless
# --no-build, where the caller staged); run on its own, RESTAGE after editing
# src/, or this measures the previous build.
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-heap-release-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$(test_build_dir)"
AXL_CXX="$(test_sdk_dir)/bin/axl-c++"
CXX_SRC="$PROJECT_DIR/test/integration/heap-release-cxx.cpp"
DRV_SRC="$PROJECT_DIR/test/integration/cxx-ctor-driver.cpp"
ORDER_SRC="$PROJECT_DIR/test/integration/heap-release-order.cpp"
WORK="$TEST_TMPDIR/heap-release"
mkdir -p "$WORK"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" \
    AXL_CPP=1 heap-release-test cxx-ctor-test 2>&1 | tail -1

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

echo "=== Heap Release Test ($TEST_ARCH) ==="

check "default C++ child builds" \
    "$AXL_CXX" --arch "$_native_arch" --release -DHEAP_REL_TAG='"eh"' \
        "$CXX_SRC" -o "$WORK/heap-rel-cxx-eh.efi"
check "--no-eh-frame C++ child builds" \
    "$AXL_CXX" --arch "$_native_arch" --release --no-eh-frame \
        -DHEAP_REL_TAG='"noeh"' "$CXX_SRC" -o "$WORK/heap-rel-cxx-noeh.efi"
# --no-eh-frame with RTTI: std::make_exception_ptr is real there, and writes
# the ABI exception header in front of the storage nothrow.o hands out.
check "--no-eh-frame -frtti exception_ptr child builds" \
    "$AXL_CXX" --arch "$_native_arch" --release --no-eh-frame -frtti \
        "$PROJECT_DIR/test/integration/heap-release-ehptr.cpp" \
        -o "$WORK/heap-rel-ehptr.efi"
check "C++ teardown-order child builds" \
    "$AXL_CXX" --arch "$_native_arch" --release "$ORDER_SRC" \
        -o "$WORK/heap-rel-order.efi"
check "C++ driver child builds" \
    "$AXL_CXX" --arch "$_native_arch" --release --type driver "$DRV_SRC" \
        -o "$WORK/heap-rel-cxx-drv.efi"
# The service unload stub tears down in its own order (registry sweep after
# the destructors), so it releases the heap at its own last step.
check "C++ service-driver child builds" \
    "$AXL_CXX" --arch "$_native_arch" --release --type driver \
        -DAXL_CTOR_FIXTURE_SERVICE "$DRV_SRC" -o "$WORK/heap-rel-cxx-svc.efi"
# A driver whose entry exits with an EFI error: the firmware unloads it
# without calling Unload, so its heap must go from _axl_cleanup.
check "failing-entry driver child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release --type driver \
        "$PROJECT_DIR/test/integration/heap-release-fail-drv.c" \
        -o "$WORK/heap-rel-fail-drv.efi"
# A SERVICE whose setup exits with an EFI error: its protocol was published
# before setup ran, and must not outlive the image.
check "failing-setup service child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release --type driver \
        "$PROJECT_DIR/test/integration/heap-release-svcexit.c" \
        -o "$WORK/heap-rel-svcexit.efi"
# A driver whose entry raises the TPL and exits with an EFI error: still the
# current image, so it is unloaded and must tear down like any other.
check "raised-TPL failing-entry driver child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release --type driver \
        "$PROJECT_DIR/test/integration/heap-release-tplexit.c" \
        -o "$WORK/heap-rel-tplexit.efi"
# A driver whose entry runs a child and connects, THEN exits with an error:
# the child count must be back to zero, or it keeps its heap.
check "child-running failing-entry driver child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release --type driver \
        "$PROJECT_DIR/test/integration/heap-release-childrun.c" \
        -o "$WORK/heap-rel-childrun.efi"
# A driver whose TIMER CALLBACK exits while a child it started is running:
# Exit cannot happen then (the child is current), so it must be deferred to
# when the child returns -- it used to spin at callback TPL and deadlock.
check "callback-exit driver child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release --type driver \
        "$PROJECT_DIR/test/integration/heap-release-cbexit.c" \
        -o "$WORK/heap-rel-cbexit.efi"
check "callback-exit app child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release -DHEAP_REL_APP \
        "$PROJECT_DIR/test/integration/heap-release-cbexit.c" \
        -o "$WORK/heap-rel-cbexit-app.efi"
# A driver that calls axl_exit(0) in its entry and so stays resident.
check "resident-after-axl_exit driver child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release --type driver \
        "$PROJECT_DIR/test/integration/heap-release-exit-drv.c" \
        -o "$WORK/heap-rel-exit-drv.efi"
# --minimal-runtime skips _axl_init/_axl_cleanup; its crt0 releases the heap.
check "--minimal-runtime C malloc child builds" \
    "$(test_sdk_dir)/bin/axl-cc" --arch "$_native_arch" --release \
        --minimal-runtime=noargs "$PROJECT_DIR/test/integration/heap-release-child.c" \
        -o "$WORK/heap-rel-c-min.efi"

test_add_efi "$NATIVE_DIR/heap-release-test.efi"
test_add_efi "$NATIVE_DIR/heap-rel-c.efi"
test_add_efi "$NATIVE_DIR/heap-rel-leak.efi"
test_add_efi "$NATIVE_DIR/heap-rel-sleep.efi"
test_add_efi "$NATIVE_DIR/cxx-ctor-driver.efi" "heap-rel-dbg-drv.efi"
for f in heap-rel-cxx-eh.efi heap-rel-cxx-noeh.efi heap-rel-cxx-drv.efi \
         heap-rel-cxx-svc.efi heap-rel-c-min.efi heap-rel-order.efi \
         heap-rel-exit-drv.efi heap-rel-fail-drv.efi heap-rel-ehptr.efi \
         heap-rel-svcexit.efi heap-rel-tplexit.efi heap-rel-childrun.efi \
         heap-rel-cbexit.efi heap-rel-cbexit-app.efi; do
    [[ -f "$WORK/$f" ]] && test_add_efi "$WORK/$f"
done

# The guest's commands live in test-heap-release-qemu.nsh.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 180
test_clean_log

echo "--- serial log (HEAPREL lines) ---"
{ grep -a '^HEAP' "$TEST_CLEAN_LOG" || true; } | sort | uniq -c | sed 's/^/  /'
echo "--- assertions ---"

# heaprel_field <section> <child> <launches> <field> -- one number from the
# child's line. <section> is `load` or `driver`, telling apart the two runs of
# the same driver by the HEAPREL_STARTED marker between them; `app` for any
# other line.
heaprel_field() {
    local section="$1" child="$2" n="$3" field="$4" log
    case "$section" in
        load)   log=$(sed -n '1,/^HEAPREL_STARTED/p' "$TEST_CLEAN_LOG") ;;
        driver) log=$(sed -n '/^HEAPREL_STARTED/,$p' "$TEST_CLEAN_LOG") ;;
        *)      log=$(cat "$TEST_CLEAN_LOG") ;;
    esac
    grep -a "^HEAPREL $child launches=$n " <<< "$log" | head -1 \
        | sed -n "s/.* $field=\\(-\\{0,1\\}[0-9]*\\).*/\\1/p"
}

# expect_app <child> <pages per launch> -- per-launch EXACT, total under one
# page per launch past it. Measured RED and GREEN, the firmware's own pool
# grows by a few pages once per boot (321 for 5 x 64; 0-3 for the children,
# landing on whichever runs first), which is one-off noise. A cost of one page
# or more per launch shows as at least 5 over 5 launches.
expect_app() {
    local child="$1" want="$2" per total
    per=$(heaprel_field app "$child" 5 per_launch)
    total=$(heaprel_field app "$child" 5 total)
    [[ -n "$per" && -n "$total" && "$per" -eq "$want" ]] \
        && (( total >= want * 5 && total <= want * 5 + 4 ))
}

# The control first: if the sampler cannot see 64 pages kept per launch, every
# zero below is the sampler's blindness, not the fix.
check "control: a child that keeps 64 pages reads as exactly 64 per launch" \
    expect_app heap-rel-leak.efi 64
for child in heap-rel-c.efi heap-rel-cxx-eh.efi heap-rel-cxx-noeh.efi \
             heap-rel-c-min.efi heap-rel-order.efi; do
    check "$child gives back every page it took" expect_app "$child" 0
done

# expect_driver <child> -- started minus load-only, over 10 cycles each, at
# most 8. Measured GREEN: 2-3 (each run drifts by a page or two, and the
# load-path cost wobbles too, so 3 sat ON a tighter bound). 8 still fails any
# cost of a whole page or more per start -- that reads as >= 10 -- and the
# defect it guards is 256 per start, 2560 here.
expect_driver() {
    expect_driver_n "$1" 10
}

# expect_driver_n <child> <cycles> -- the same, over a given cycle count.
expect_driver_n() {
    local child="$1" n="$2" load run
    load=$(heaprel_field load "$child" "$n" total)
    run=$(heaprel_field driver "$child" "$n" total)
    echo "    $child: load-only ${load:-?}, started ${run:-?} (pages over $n cycles)"
    [[ -n "$load" && -n "$run" ]] && (( run - load <= 8 ))
}
check "a C++ driver gives back its heap on unload (started - load-only <= 8)" \
    expect_driver heap-rel-cxx-drv.efi
check "a C++ service driver gives back its heap on unload" \
    expect_driver heap-rel-cxx-svc.efi
check "a DEBUG driver gives back AxlMem's free quarantine on unload" \
    expect_driver heap-rel-dbg-drv.efi
# The `fail` run reads from the second section like `driver`.
check "a driver whose entry exits with an EFI error gives back its heap" \
    expect_driver heap-rel-fail-drv.efi
check "a service whose setup exits with an EFI error gives back its heap" \
    expect_driver heap-rel-svcexit.efi
check "a driver exiting with an error at a TPL it raised gives back its heap" \
    expect_driver heap-rel-tplexit.efi
check "an entry that ran a child and connected, then failed, gives back its heap" \
    expect_driver heap-rel-childrun.efi
# That entry fails by design, so its heap check alone would pass with a child
# that never ran: 11 starts (a warm-up and 10), each running its child.
check "childrun: every start ran its child (11 x)" \
    test "$(grep -ac '^CHILDRUN child ran$' "$TEST_CLEAN_LOG")" -eq 11
check "childrun: no start failed to run its child" \
    test "$(grep -ac '^CHILDRUN image_run failed$' "$TEST_CLEAN_LOG")" -eq 0
# The callback-exit case, 6 starts (a warm-up and 5): every callback fired
# DURING the child, every child finished, no entry ran past the child, and
# the deferred exit unloaded the image with its heap.
# 12 = 6 driver starts + 6 app launches.
check "callback-exit: the callback fired while the child ran (12 x)" \
    test "$(grep -ac '^CBEXIT exiting from a callback$' "$TEST_CLEAN_LOG")" -eq 12
check "callback-exit: the child ran to completion every time (12 x)" \
    test "$(grep -ac '^SLEEP done$' "$TEST_CLEAN_LOG")" -eq 12
check "callback-exit: the exit took effect when the child returned" \
    test "$(grep -ac '^CBEXIT child returned without exit$' "$TEST_CLEAN_LOG")" -eq 0
check "callback-exit: every child actually ran" \
    test "$(grep -ac '^CBEXIT image_run failed$' "$TEST_CLEAN_LOG")" -eq 0
# The driver's second callback (6 starts) asked again, with a SUCCESS status;
# the first request's error still decided (the driver unloaded -- below).
check "callback-exit: the later request ran and was not the one obeyed (6 x)" \
    test "$(grep -ac '^CBEXIT second request$' "$TEST_CLEAN_LOG")" -eq 6
check "callback-exit: the abandoned callback left no loop depth behind" \
    test "$(grep -ac 'synchronous wait invoked from inside a loop callback' "$TEST_CLEAN_LOG")" -eq 0
check "callback-exit: the deferred exit unloaded the driver with its heap" \
    expect_driver_n heap-rel-cbexit.efi 5
check "callback-exit: an APP's deferred exit gives back its heap too" \
    expect_app heap-rel-cbexit-app.efi 0
check "...and leaves no protocol behind on the unloaded image" \
    grep -aFxq "HEAPSVC heap-rel-svcexit installed=no" "$TEST_CLEAN_LOG"
# The pool. A --no-eh-frame image can never use libsupc++'s emergency pool,
# so it must not carry it -- and, never mallocing, must not take an arena at
# all. Six launches each (a warm-up and five); the default build is the
# control that the probe can see an arena.
check "a --no-eh-frame image takes no C heap before main (6 x none)" \
    test "$(grep -ac '^HEAPARENA noeh none$' "$TEST_CLEAN_LOG")" -eq 6
check "control: a default C++ image has taken one by then (6 x taken)" \
    test "$(grep -ac '^HEAPARENA eh taken$' "$TEST_CLEAN_LOG")" -eq 6

# exception_ptr in a --no-eh-frame -frtti image: the storage must carry the
# 128-byte ABI header libsupc++ writes, or the sentinel before it is hit.
check "--no-eh-frame exception storage carries the ABI header (6 x ok)" \
    test "$(grep -ac '^EHPTR ok$' "$TEST_CLEAN_LOG")" -eq 6
check "...and nothing landed outside an allocation" \
    test "$(grep -ac '^EHPTR corrupt$' "$TEST_CLEAN_LOG")" -eq 0

# The order child runs 6 times (a warm-up and 5 measured); each prints its
# verdict once, from inside teardown.
check "teardown-order: the heap was still there for every late destructor (6 x ok)" \
    test "$(grep -ac '^HEAPORDER ok$' "$TEST_CLEAN_LOG")" -eq 6
check "teardown-order: no late destructor found the heap gone" \
    test "$(grep -ac '^HEAPORDER refused$' "$TEST_CLEAN_LOG")" -eq 0

# The resident driver. While it lives it must still own its arena (>= 256
# pages: the 1 MiB first commit); unload must give it back. Measured GREEN
# 3-4 after unload -- one load-path cycle plus the one-off noise -- so the
# bound is 16: well clear of that, and of the 256 an arena kept would show.
# `|| true`: no HEAPHOLD line at all (the guest faulted before printing it, as
# it did RED) must reach the checks below as a FAIL, not end this script under
# `set -e -o pipefail` with no verdict printed.
HOLD=$(grep -a '^HEAPHOLD heap-rel-exit-drv.efi ' "$TEST_CLEAN_LOG" | head -1 || true)
HELD=$(sed -n 's/.* held=\(-\{0,1\}[0-9]*\) .*/\1/p' <<< "$HOLD")
AFTER=$(sed -n 's/.* after_unload=\(-\{0,1\}[0-9]*\)$/\1/p' <<< "$HOLD")
echo "    heap-rel-exit-drv.efi: held ${HELD:-?} while resident, ${AFTER:-?} after unload"
check "a driver resident after axl_exit still owns its heap (>= 256 pages)" \
    test -n "$HELD" -a "${HELD:-0}" -ge 256
check "...and gives it back on unload (<= 16 pages left)" \
    test -n "$AFTER" -a "${AFTER:-999}" -le 16
check "the shell survived every case" grep -aq '^HEAPREL_DONE' "$TEST_CLEAN_LOG"

echo ""
echo "heap-release ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
