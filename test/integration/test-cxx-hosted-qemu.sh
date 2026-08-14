#!/bin/bash
# test-meta: arch=both needs= est=110 local-only=0
# test-cxx-hosted-qemu.sh — the standard containers under UEFI, end to end.
#
# THE NAME IS HISTORICAL. There is no hosted MODE any more: task T3 dropped
# -ffreestanding from the C++ line in all three build paths, so
# <vector>/<string>/<map>/<unordered_map> compile with no flag and
# `axl-c++ --hosted` is an accepted no-op. Kept as the filename because this is
# still the containers-under-UEFI suite and renaming it would break the
# ratchet's per-test history for no gain.
#
# What this asserts, and why each one is here rather than implied:
#
#   1. The containers compile with NO FLAG, and --hosted is now REJECTED
#      with a message naming its removal. This assertion is inverted from what it was
#      ("without --hosted the containers are refused"), which was the honest
#      test while two modes existed. What can regress now is -ffreestanding
#      creeping back onto the C++ line, and that shows up here as a refusal at
#      bits/requires_hosted.h -- the same failure, now the failing case.
#
#      SCOPE, because "three build paths" would be a false claim: this drives
#      axl-cc only. The Makefile path is self-covering (axl-cxx-rehash.cpp
#      includes <unordered_map>, so the library simply fails to build). The
#      GENERATED CMAKE PACKAGE is covered by nothing at all -- no test and no
#      CI step ever configures find_package(axl) -- and check-flag-parity is a
#      whole-file substring test that the C flag set already satisfies. That
#      gap predates T3 and is worth closing on its own.
#   2. libaxl-cxx.a defines the five std::__throw_* symbols. Under
#      -fno-exceptions the container headers still CALL them; if libstdc++.a
#      satisfies them instead, its functexcept.o arrives compiled WITH
#      exceptions and drags in the unwinder.
#   3. The link resolves __throw_length_error and _Prime_rehash_policy's two
#      out-of-line members FROM libaxl-cxx.a, proven with ld -y rather than
#      inferred from "it linked". Both members or neither: they share
#      _M_next_resize, so a build that takes one from each side links cleanly
#      and then disagrees with itself about when to rehash.
#   4. No ungated AVX in the produced image. UEFI boots with CR4.OSXSAVE
#      clear, and this host's libstdc++ hashtable_c++0x.o carries 49 VEX
#      instructions -- the #UD that gates x64 (docs/AXL-Cxx-Stdlib-Handoff.md
#      section 3). This is the assertion that the member was not pulled.
#   5. The image RUNS and prints exact lines. Compile is not link; link is not
#      run -- each of those three steps produced a wrong conclusion in the
#      session that led to this test.
#   6. The __throw_* stubs actually halt, with a diagnosable message, rather
#      than falling through into code the optimizer proved unreachable.
#   7. A DEFAULT C++ link names no libstdc++.a, read off the link line rather
#      than inferred. -frtti is the sole exception and is opt-in. That is the
#      AXL-Cxx-Design.md §8 constraint: redistributing the GCC runtime is the
#      one act the Runtime Library Exception does not cover.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-hosted-qemu.sh [X64|AARCH64|both]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

AXL_CXX="$PROJECT_DIR/out/bin/axl-c++"
SRC="$SCRIPT_DIR/cxx-hosted-selftest.cpp"
THROW_SRC="$SCRIPT_DIR/cxx-hosted-throw.cpp"
BADALLOC_SRC="$SCRIPT_DIR/cxx-hosted-badalloc.cpp"
NEWFORMS_SRC="$SCRIPT_DIR/cxx-new-forms.cpp"
# The SDK example, run rather than merely compiled: axl::result and
# axl::err are the C++ layer's entire public surface and had NO consumer
# at all until this example existed -- check-examples compiles
# sdk/examples/*.cpp, but nothing included axl-cxx.hpp, so no gate could
# see the header.
ERRORS_SRC="$PROJECT_DIR/sdk/examples/cxx-errors.cpp"
CTOR_SRC="$SCRIPT_DIR/cxx-ctor-selftest.cpp"
RTTI_SRC="$SCRIPT_DIR/cxx-rtti-selftest.cpp"
WORK="$(mktemp -d -t axl-cxx-hosted.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Mangled names. Spelled out because ld -y takes the mangled form; each is
# cross-checked against nm's demangled output below, so a mangling change
# fails loudly here instead of turning every grep into a silent no-match.
THROW_SYMS=(
    _ZSt17__throw_bad_allocv
    _ZSt28__throw_bad_array_new_lengthv
    _ZSt20__throw_length_errorPKc
    _ZSt19__throw_logic_errorPKc
    _ZSt24__throw_out_of_range_fmtPKcz
)
REHASH_SYMS=(
    _ZNKSt8__detail20_Prime_rehash_policy11_M_next_bktEm
    _ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmmm
)
# The helpers that replaced libstdc++'s tree.o, hash_bytes.o, list.o and
# shared_ptr.o. Listed in full rather than sampled: the argument for naming
# _M_hook is the same argument for naming the other four, and a partial list
# invites the next addition to be partial too.
TREE_SYMS=(
    _ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base
    _ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base
    _ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_
    _ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_
    _ZSt11_Hash_bytesPKvmm
    _ZNSt8__detail15_List_node_base7_M_hookEPS0_
    _ZNSt8__detail15_List_node_base9_M_unhookEv
    _ZNSt8__detail15_List_node_base11_M_transferEPS0_S1_
    _ZNSt8__detail15_List_node_base10_M_reverseEv
    _ZNSt8__detail15_List_node_base4swapERS0_S1_
    _ZNSt19_Sp_make_shared_tag5_S_eqERKSt9type_info
)

EXPECTED=(
    "cxx: vec 1 3 5 9"
    "cxx: str axl-sdk len=7"
    "cxx: map 3 apple=1 fig=2 pear=3"
    "cxx: umap 200 sq13=169 sq199=39601"
    "cxx: umap grow bkt>=200 ok lf<=mlf ok"
    "cxx: umap reserve bkt>=500 ok"
    "cxx: umap mlf 400 intact"
    "cxx: umap mlf bkt>=800 ok lf<=mlf ok"
    "cxx: umap churn 400 u[7]=8"
    "cxx: arena vec 1000 av[999]=1998"
    "cxx: arena drew ok regrew no"
    "cxx: arena align32 ok"
    "cxx: arena map 50 am[17]=51"
    "cxx: arena swap 22 11 alloc followed"
    "cxx: arena guard toobig ok overflow ok"
    "cxx: arena guard sound ok"
    "cxx: arena zero ok"
    "cxx: arena reset ok"
    "cxx: new overaligned ok"
    "cxx: nothrow null"
    # The std::_Rb_tree_* helpers, ours since libstdc++'s tree.o was dropped.
    # `shape`/`bh` read the real node graph through _Rb_tree_iterator::_M_node:
    # std::map answers every query correctly even when the tree has degenerated
    # into a list, so `order`/`find` alone cannot see a rebalancing bug. Proven
    # by sabotage -- dropping one recolour in the erase fixup leaves
    # `order ok find ok` and trips only `shape`/`bh`.
    "cxx: rb steps ok size ok shape ok order ok rev ok find ok bh ok"
    "cxx: rb multi ok set ok"
    # std::list and shared_ptr, ours since libstdc++'s list.o was dropped --
    # the member whose landing pads dragged in the "tier 2" _Unwind_* cascade.
    # A list is a RING, so `ring` (walked forwards AND backwards) is a
    # different claim from `order`: a broken _M_transfer leaves a list that
    # iterates forward correctly and is corrupt in reverse.
    "cxx: list steps ok size ok ring ok order ok sort ok rev ok splice ok selfsplice ok swap ok both ok merge ok"
    "cxx: shared ok"
    "cxx: ceil 2 3 -2 1"
    "cxx: done"
)

pass=0
fail=0
skipped=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "  PASS: $2"; pass=$((pass + 1))
    else echo "  FAIL: $2"; fail=$((fail + 1)); fi
}

run_one() {
    local arch="$1" cc_arch out lib_dir efi so log rc
    case "$arch" in
        X64)     cc_arch="x64";  out="$PROJECT_DIR/out/native-x64" ;;
        AARCH64) cc_arch="aa64"; out="$PROJECT_DIR/out/native-aa64" ;;
    esac
    lib_dir="$PROJECT_DIR/out/lib/axl/$cc_arch"

    echo "=== cxx-hosted ($arch) ==="
    if [[ ! -x "$AXL_CXX" || ! -f "$lib_dir/libaxl-cxx.a" ]]; then
        echo "  SKIP ($arch): no staged C++ SDK at $lib_dir/libaxl-cxx.a"
        echo "        run: scripts/install.sh --arch $cc_arch --cpp"
        skipped=$((skipped + 1))
        return
    fi

    # ---------------------------------------------------------------
    # 0. The staged SDK is a SECOND TREE, and it must be current.
    # ---------------------------------------------------------------
    # axl-c++ compiles against out/include/axl-sdk and links out/lib, so an
    # edit to include/axl that has not been re-staged means everything below
    # exercises the PREVIOUS build. That is not hypothetical: while this test
    # was being written, a restored sabotage lived on in the staged headers
    # and turned the next sabotage's verdict into a false positive.
    #
    # Compared by content, not mtime -- install.sh deliberately avoids mtime
    # churn (install -C), so an mtime test would report drift that isn't there.
    diff -rq "$PROJECT_DIR/include/axl" \
             "$PROJECT_DIR/out/include/axl-sdk/axl" >"$WORK/stage.diff" 2>&1
    check "$?" "$arch: staged headers match include/axl (else: install.sh --cpp)"
    [[ -s "$WORK/stage.diff" ]] && sed 's/^/      /' "$WORK/stage.diff" | head -5

    # ---------------------------------------------------------------
    # 1. The containers compile with NO FLAG AT ALL.
    #
    # This assertion is INVERTED from what it was, and the inversion IS task
    # T3 (AXL-Cxx-Design.md §6a-PLAN). It used to read "without --hosted the
    # containers are refused", which was the honest test while two compile
    # modes existed: a passing hosted build proved nothing about the flag
    # unless the flag was load-bearing.
    #
    # There is one mode now, so the discriminating question changed. What
    # could regress is -ffreestanding creeping back onto the C++ line in any
    # of the three build paths, and that shows up here as a refusal at
    # bits/requires_hosted.h -- the same failure, now the failing case.
    # ---------------------------------------------------------------
    "$AXL_CXX" --arch "$cc_arch" -c "$SRC" -o "$WORK/free.o" \
        >"$WORK/free.log" 2>&1
    rc=$?
    check "$rc" "$arch: the containers compile with no flag (rc=$rc)"
    [[ "$rc" -ne 0 ]] && grep -i "freestanding\|requires_hosted" "$WORK/free.log" \
        | head -3 | sed 's/^/      /'

    # ---------------------------------------------------------------
    # 1b. --hosted is REJECTED, and the message says why.
    #
    # A flag that selects nothing should not be quietly tolerated: the
    # failure mode of tolerating it is a build script that keeps passing a
    # meaningless option for years. Asserting the MESSAGE too, not just the
    # exit status -- axl-cc already rejects every unknown --option, so a
    # status-only check would pass if this arm were deleted outright and
    # would prove nothing about the caller being told what happened.
    # ---------------------------------------------------------------
    "$AXL_CXX" --arch "$cc_arch" --hosted -c "$SRC" -o "$WORK/hosted.o" \
        >"$WORK/hosted.log" 2>&1
    rc=$?
    [[ "$rc" -ne 0 ]] && grep -q "was removed" "$WORK/hosted.log"
    check "$?" "$arch: --hosted is rejected, naming the removal (rc=$rc)"

    # It must not have produced output on the way to failing.
    [[ ! -f "$WORK/hosted.o" ]]
    check "$?" "$arch: the rejected --hosted build wrote no object"

    # ---------------------------------------------------------------
    # 2. libaxl-cxx.a carries the five __throw_* definitions.
    # ---------------------------------------------------------------
    local nm_bin="nm"
    [[ "$arch" == "AARCH64" ]] && nm_bin="aarch64-linux-gnu-nm"
    "$nm_bin" --defined-only "$lib_dir/libaxl-cxx.a" >"$WORK/cxxlib.nm" 2>/dev/null
    local sym missing=""
    for sym in "${THROW_SYMS[@]}" "${REHASH_SYMS[@]}" "${TREE_SYMS[@]}"; do
        grep -q " $sym\$" "$WORK/cxxlib.nm" || missing="$missing $sym"
    done
    [[ -z "$missing" ]]
    check "$?" "$arch: libaxl-cxx.a defines all $((${#THROW_SYMS[@]} + ${#REHASH_SYMS[@]} + ${#TREE_SYMS[@]})) libstdc++ hook symbols${missing:+ (missing:$missing)}"

    # The mangled names above must still BE these functions. Without this a
    # renamed mangling turns every grep into a silent no-match.
    #
    # Via a file, not a pipe: `set -o pipefail` is on, and `grep -q` exits on
    # the first match, SIGPIPEing nm -- so `nm | grep -q` reports FAILURE on
    # the runs where it found what it was looking for.
    "$nm_bin" -C --defined-only "$lib_dir/libaxl-cxx.a" >"$WORK/cxxlib.dm" 2>/dev/null
    grep -qF "std::__throw_out_of_range_fmt(char const*, ...)" "$WORK/cxxlib.dm"
    check "$?" "$arch: the mangled names still demangle to the std:: functions"

    # ---------------------------------------------------------------
    # 3. Hosted build, with ld -y proving WHO satisfied each symbol.
    # ---------------------------------------------------------------
    efi="$WORK/cxx-hosted-$cc_arch.efi"
    so="${efi%.efi}.so"
    local trace=()
    for sym in "${THROW_SYMS[0]}" _ZSt20__throw_length_errorPKc "${REHASH_SYMS[@]}"; do
        trace+=("-Wl,-y,$sym")
    done
    "$AXL_CXX" --arch "$cc_arch" --release "${trace[@]}" \
        "$SRC" -o "$efi" >"$WORK/build.log" 2>&1
    rc=$?
    [[ "$rc" -eq 0 && -f "$efi" ]]
    check "$?" "$arch: builds the container fixture (rc=$rc)"
    if [[ ! -f "$efi" ]]; then
        sed 's/^/      /' "$WORK/build.log" | tail -25
        return
    fi

    # Every traced symbol must be DEFINED BY libaxl-cxx.a. ld prints
    # "<file>: definition of <sym>" for the archive member that supplied it.
    local from_us=1
    for sym in "${REHASH_SYMS[@]}" _ZSt20__throw_length_errorPKc; do
        if ! grep -E "libaxl-cxx\.a.*: definition of $sym\$" "$WORK/build.log" >/dev/null; then
            from_us=0
            echo "      $sym came from: $(grep -E ": definition of $sym\$" "$WORK/build.log" | head -1 || echo '<nothing>')"
        fi
    done
    [[ "$from_us" -eq 1 ]]
    check "$?" "$arch: libaxl-cxx.a — not libstdc++.a — defined the hook symbols"

    # ---------------------------------------------------------------
    # 3b. libstdc++.a is not linked AT ALL.
    # ---------------------------------------------------------------
    # Not a size optimisation: redistributing libstdc++ is the one act the
    # GCC Runtime Library Exception does not cover, so not linking it is what
    # lets the SDK be self-contained without GPL-3 obligations. It also drops
    # the --hosted prerequisite that a matching libstdc++-static be installed.
    #
    # Asserted on the BUILD LOG (ld -t names every archive it opens) rather
    # than on the image, because an archive contributing zero members leaves
    # no trace in the output -- which is exactly the state that regressed
    # silently before, when tree.o was the only member still being pulled.
    "$AXL_CXX" --arch "$cc_arch" --release -Wl,-t "$SRC" \
        -o "$WORK/tracelink-$cc_arch.efi" >"$WORK/tracelink.log" 2>&1
    trace_rc=$?

    # The exit status FIRST. Discarding it made both checks below green on a
    # link that produced no image at all: a failed link names no libstdc++.a
    # (check 1 passes) and ld's partial -t listing still names libaxl-cxx.a
    # (the control passes too). An absence proves nothing about a build that
    # did not happen.
    [[ "$trace_rc" -eq 0 && -f "$WORK/tracelink-$cc_arch.efi" ]]
    check "$?" "$arch: the -Wl,-t probe link SUCCEEDED (rc=$trace_rc)"

    ! grep -q "libstdc++\.a" "$WORK/tracelink.log"
    check "$?" "$arch: the hosted link names no libstdc++.a"

    # Positive control: -t must actually have produced a listing, or the
    # absence above proves nothing.
    grep -q "libaxl-cxx\.a" "$WORK/tracelink.log"
    check "$?" "$arch: ld -t listing is present (control for the check above)"

    # ---------------------------------------------------------------
    # 4. No ungated AVX reached the image.
    # ---------------------------------------------------------------
    python3 "$PROJECT_DIR/scripts/check-no-avx.py" "$so" >"$WORK/avx.log" 2>&1
    check "$?" "$arch: no ungated VEX/EVEX in the hosted image"
    [[ -s "$WORK/avx.log" ]] && sed 's/^/      /' "$WORK/avx.log" | head -5

    # ---------------------------------------------------------------
    # 5. It runs, and prints exactly these lines.
    # ---------------------------------------------------------------
    log="$WORK/run-$cc_arch.log"
    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --timeout 45 "$efi" >"$log" 2>&1
    # Serial output carries CR; strip it so grep -Fx compares the line itself.
    tr -d '\r' < "$log" > "$log.clean"
    local line
    for line in "${EXPECTED[@]}"; do
        grep -Fxq "$line" "$log.clean"
        check "$?" "$arch: output line — $line"
    done

    # ---------------------------------------------------------------
    # 5b. Every new/delete form LINKS.
    # ---------------------------------------------------------------
    # These operators live in libaxl-cxx.a, so a consumer writing plain C++
    # hits them without opting into anything. Two of them had no definition at
    # all until this fixture existed, and both failed only at LINK:
    # `new (std::nothrow) T` needs the std::nothrow OBJECT (libsupc++, which
    # firmware does not link), and an over-aligned `new` calls a different
    # operator entirely.
    #
    # This ran TWICE, once per mode, until T3 removed the modes. Kept as one
    # run rather than two identical ones: a second PASS line for a distinction
    # that no longer exists is a phantom assertion, and the ratchet counts it.
    "$AXL_CXX" --arch "$cc_arch" --release \
        "$NEWFORMS_SRC" -o "$WORK/newforms.efi" \
        >"$WORK/newforms.log" 2>&1
    rc=$?
    check "$rc" "$arch: every new/delete form links"
    [[ "$rc" -eq 0 ]] || grep -E "undefined reference" "$WORK/newforms.log" \
        | sed 's/^/      /' | head -4

    # ---------------------------------------------------------------
    # 5c. -frtti links with no mode flag, and is the ONLY thing that
    #     puts libstdc++.a on the link line.
    # ---------------------------------------------------------------
    # libstdc++.a carries the __cxxabiv1 type_info vtables and __dynamic_cast,
    # so -frtti needs nothing from us beyond __stack_chk_fail.
    #
    # This pair used to read "works hosted, honestly fails freestanding" --
    # the freestanding half is gone with T3, since there is no mode to fail
    # in. What replaces it is the property that actually mattered underneath:
    # RTTI is OPT-IN, and the DEFAULT link names no GCC runtime library at
    # all. That is the AXL-Cxx-Design.md §8 constraint -- redistributing the
    # runtime is the one act the Runtime Library Exception does not cover --
    # so a default build silently acquiring libstdc++.a is the regression
    # worth catching, not a mode that no longer exists.
    "$AXL_CXX" --arch "$cc_arch" --release -frtti \
        "$RTTI_SRC" -o "$WORK/rtti-$cc_arch.efi" >"$WORK/rtti.log" 2>&1
    check "$?" "$arch: -frtti links with no mode flag"
    [[ -s "$WORK/rtti.log" ]] && grep -oP "undefined reference to .\K[^']+" \
        "$WORK/rtti.log" | head -3 | sed 's/^/      /'

    # The negative, read off the LINK LINE rather than inferred from success:
    # --verbose prints the ld invocation, and libstdc++.a must appear on the
    # -frtti one and on no other.
    #
    # BOTH halves need a positive control, and for DIFFERENT reasons -- an
    # absence proves nothing if the link never ran, and a presence proves
    # nothing if the string came from an error message rather than a command
    # line. axl-cc's own "-frtti needs libstdc++.a" diagnostic contains the
    # literal string, so the grep below would pass on the exact failure it is
    # meant to catch.
    "$AXL_CXX" --arch "$cc_arch" --release --verbose -frtti \
        "$RTTI_SRC" -o "$WORK/rtti-v.efi" >"$WORK/rtti-v.log" 2>&1
    rc=$?
    [[ "$rc" -eq 0 && -f "$WORK/rtti-v.efi" ]] \
        && grep -q -- "-frtti.*libstdc++\.a\|libstdc++\.a" "$WORK/rtti-v.log" \
        && grep -q "libaxl-cxx\.a" "$WORK/rtti-v.log"
    check "$?" "$arch: -frtti DOES name libstdc++.a on a link that succeeded (rc=$rc)"

    "$AXL_CXX" --arch "$cc_arch" --release --verbose \
        "$SRC" -o "$WORK/nortti-v.efi" >"$WORK/nortti-v.log" 2>&1
    rc=$?
    # The control: libaxl-cxx.a must BE on this line. Without it, a link that
    # died before ld ran -- or a --verbose that printed nothing -- reads as
    # "no libstdc++.a" and passes.
    [[ "$rc" -eq 0 && -f "$WORK/nortti-v.efi" ]] \
        && grep -q "libaxl-cxx\.a" "$WORK/nortti-v.log" \
        && ! grep -q "libstdc++\.a" "$WORK/nortti-v.log"
    check "$?" "$arch: a default C++ link names NO libstdc++.a (self-contained, rc=$rc)"

    # Run-asserted on BOTH arches. It was x64-only until the aa64 linker
    # script was fixed: an RTTI link there produced a linker-synthesized
    # `.rela.dyn` AND the script-placed `.rela` at non-contiguous addresses,
    # while DT_RELA pointed at the first and DT_RELASZ counted both, so the
    # crt0's walk ran off the end and applied the following bytes as
    # relocations. Naming the output section `.rela.dyn` makes ld absorb its
    # own internal section instead of orphaning it.
    #
    # `make check-reloc-coverage` is the structural guard; this is the
    # behavioural one. RTTI is the workload that first split the table, so it
    # stays the workload that proves it is not split again.
    if [[ -f "$WORK/rtti-$cc_arch.efi" ]]; then
        timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
            --arch "$arch" --timeout 45 "$WORK/rtti-$cc_arch.efi" \
            >"$WORK/rttirun.log" 2>&1
        tr -d '\r' < "$WORK/rttirun.log" > "$WORK/rttirun.clean"
        grep -Fxq "rtti: name=7Derived cast=42 neg=1" "$WORK/rttirun.clean"
        check "$?" "$arch: typeid and dynamic_cast run correctly under UEFI"
    fi

    # ---------------------------------------------------------------
    # 5d. The axl::result example RUNS. Freestanding: axl::result is
    #     std::expected, which is in the C++23 freestanding subset.
    # ---------------------------------------------------------------
    "$AXL_CXX" --arch "$cc_arch" --release "$ERRORS_SRC" \
        -o "$WORK/errors-$cc_arch.efi" >"$WORK/errors.log" 2>&1
    check "$?" "$arch: the axl::result example builds freestanding"
    if [[ -f "$WORK/errors-$cc_arch.efi" ]]; then
        timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
            --arch "$arch" --timeout 45 "$WORK/errors-$cc_arch.efi" \
            >"$WORK/errrun.log" 2>&1
        tr -d '\r' < "$WORK/errrun.log" > "$WORK/errrun.clean"
        local eline
        for eline in \
            '  "443"      ok    port=443' \
            '  ""         error status=-4 fallback=8080' \
            'cxx-errors: pair ok=1 packed=0x005001bb' \
            'cxx-errors: arena released by scope exit' \
            'cxx-errors: done'; do
            grep -Fxq "$eline" "$WORK/errrun.clean"
            check "$?" "$arch: axl::result output — ${eline# *}"
        done
    fi

    # ---------------------------------------------------------------
    # 5e. C++ global constructors run before main.
    # ---------------------------------------------------------------
    # They did NOT, for as long as the C++ layer existed: --gc-sections
    # collected .init_array because the linker scripts placed it without
    # KEEP(), and nothing references an .init_array entry by design. The crt0
    # walker found start == end and ran nothing, silently.
    "$AXL_CXX" --arch "$cc_arch" --release "$CTOR_SRC" \
        -o "$WORK/ctor-$cc_arch.efi" >"$WORK/ctor.log" 2>&1
    check "$?" "$arch: the global-constructor fixture builds"
    if [[ -f "$WORK/ctor-$cc_arch.efi" ]]; then
        timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
            --arch "$arch" --timeout 45 "$WORK/ctor-$cc_arch.efi" \
            >"$WORK/ctorrun.log" 2>&1
        tr -d '\r' < "$WORK/ctorrun.log" > "$WORK/ctorrun.clean"
        local cline
        for cline in 'ctor: First ran' 'ctor: Second ran' \
                     'ctor: magic=0x1BCD order=1,2 count=2' 'ctor: done'; do
            grep -Fxq "$cline" "$WORK/ctorrun.clean"
            check "$?" "$arch: global ctor — $cline"
        done
    fi

    # ---------------------------------------------------------------
    # 6. The two halt paths: reached, named, and NOT returned from.
    # ---------------------------------------------------------------
    halt_fixture "$arch" "$cc_arch" out-of-range "$THROW_SRC" \
        "cxx-throw: about to call vector::at(99) on a 3-element vector" \
        "__throw_out_of_range_fmt" \
        "cxx-throw: UNREACHABLE"
    halt_fixture "$arch" "$cc_arch" bad-alloc "$BADALLOC_SRC" \
        "cxx-badalloc: about to allocate with OOM injected" \
        "__throw_bad_alloc" \
        "cxx-badalloc: UNREACHABLE"
}

# Build a fixture that is SUPPOSED to halt, run it, and assert three things
# that are easy to conflate: it got as far as the call, the stub identified
# itself rather than dying mutely, and it did not RETURN. The third is the one
# that matters most -- gcc compiles the code after a throw site assuming it is
# unreachable, so a stub that returns resumes in a state nothing modelled.
halt_fixture() {
    local arch="$1" cc_arch="$2" name="$3" src="$4"
    local reached="$5" marker="$6" unreachable="$7"
    local efi="$WORK/cxx-$name-$cc_arch.efi" rc

    "$AXL_CXX" --arch "$cc_arch" --release "$src" -o "$efi" \
        >"$WORK/$name-build.log" 2>&1
    rc=$?
    [[ "$rc" -eq 0 && -f "$efi" ]]
    check "$?" "$arch: builds the $name fixture (rc=$rc)"
    [[ -f "$efi" ]] || return

    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --timeout 45 "$efi" >"$WORK/$name-run.log" 2>&1
    tr -d '\r' < "$WORK/$name-run.log" > "$WORK/$name-run.clean"

    grep -Fxq "$reached" "$WORK/$name-run.clean"
    check "$?" "$arch: $name fixture reached the failing call"
    grep -qF "axl-cxxabi" "$WORK/$name-run.clean" \
        && grep -qF "$marker" "$WORK/$name-run.clean"
    check "$?" "$arch: $name named itself ($marker) instead of halting mutely"
    ! grep -qF "$unreachable" "$WORK/$name-run.clean"
    check "$?" "$arch: $name did not return into unreachable code"
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

if (( skipped == ${#ARCHES[@]} )); then
    echo "cxx-hosted: every arch SKIPPED — nothing was verified."
    echo "  Stage the C++ SDK first: scripts/install.sh --arch all --cpp"
    exit 2
fi

echo "cxx-hosted: $pass passed, $fail failed, $skipped arch(es) skipped"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
