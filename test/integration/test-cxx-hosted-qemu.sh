#!/bin/bash
# test-meta: arch=both needs= est=110 local-only=0
# test-cxx-hosted-qemu.sh — the standard containers under UEFI, end to end.
#
# THE NAME IS HISTORICAL. There is no hosted MODE any more: task T3 dropped
# -ffreestanding from the C++ line in all three build paths, so
# <vector>/<string>/<map>/<unordered_map> compile with no flag and
# `axl-c++ --hosted` hard-errors (asserted below). Kept as the filename because this is
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
#      axl-cc only. The Makefile path is covered by `make check-examples`,
#      which compiles sdk/examples/containers.cpp with CXXFLAGS_EXAMPLE --
#      derived from the Makefile's own CXXFLAGS_BASE by filter-out -- and that
#      source includes <map>/<string>/<vector>, none of them in the C++23
#      freestanding subset. (NOT cxx-streams-selftest, which an earlier
#      version of this note named: everything it includes IS in the
#      freestanding subset, so it compiles cleanly WITH -ffreestanding --
#      verified -- and cannot catch the regression. The genuinely
#      self-covering source used to be axl-cxx-rehash.cpp, deleted by P4.) The
#      GENERATED CMAKE PACKAGE is covered by nothing at all -- no test and no
#      CI step ever configures find_package(axl) -- and check-flag-parity is a
#      whole-file substring test that the C flag set already satisfies. That
#      gap predates T3 and is worth closing on its own.
#   2. libstdc++.a defines the hook symbols the container headers call, and
#      the mangled spellings this file greps for still ARE those functions.
#      INVERTED AT P4: these used to come from libaxl-cxx.a, which existed
#      because functexcept.o arrives compiled WITH exceptions and drags in
#      the unwinder. P4 accepts that cascade in exchange for iostreams, so
#      the archive is gone and the toolchain is the provider.
#   3. The link resolves __throw_length_error and _Prime_rehash_policy's two
#      out-of-line members FROM libstdc++.a, proven with ld -y rather than
#      inferred from "it linked". Both members or neither: they share
#      _M_next_resize, so a build that took one from each side would link
#      cleanly and then disagree with itself about when to rehash.
#   4. No ungated AVX in the produced image. UEFI boots with CR4.OSXSAVE
#      clear, and the DISTRO libstdc++'s hashtable_c++0x.o carries 49 VEX
#      instructions -- the #UD that gates x64 (docs/AXL-Cxx-Stdlib-Handoff.md
#      section 3). AXL's own substitutes used to be the answer; since P4 the
#      answer is that the hermetic toolchain's libstdc++ is measured clean,
#      and `make check-no-avx` scans that archive. This asserts the produced
#      IMAGE, which is the claim that actually matters.
#   5. The image RUNS and prints exact lines. Compile is not link; link is not
#      run -- each of those three steps produced a wrong conclusion in the
#      session that led to this test.
#   6. The __throw_* stubs actually halt, with a diagnosable message, rather
#      than falling through into code the optimizer proved unreachable.
#   7. EVERY C++ link names libstdc++.a, read off the link line rather than
#      inferred, and -frtti is no longer special. INVERTED AT P4. The old
#      assertion protected AXL-Cxx-Design.md §8 -- redistributing the GCC
#      runtime is the one act the Runtime Library Exception does not cover --
#      and that constraint is untouched, because the SDK still ships no
#      libstdc++: axl-cc resolves the CONSUMER's installed copy through
#      `$GXX_BIN -print-file-name`. Since P3 put libc/libm/libgcc on every
#      link, that toolchain was already a hard prerequisite for any link at
#      all, so P4 adds no install step.
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

AXL_CXX="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-c++"
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
HANDLE_SRC="$SCRIPT_DIR/cxx-handle-selftest.cpp"
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
        X64)     cc_arch="x64";  out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)" ;;
        AARCH64) cc_arch="aa64"; out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)" ;;
    esac
    lib_dir="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/lib/axl/$cc_arch"

    echo "=== cxx-hosted ($arch) ==="
    # Keyed on a cxxrt OBJECT: P4 deleted libaxl-cxx.a, so keying the skip on
    # that would make this whole suite skip forever, silently.
    if [[ ! -x "$AXL_CXX" || ! -f "$lib_dir/axl-cxxrt-terminate.o" ]]; then
        echo "  SKIP ($arch): no staged C++ SDK at $lib_dir"
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
             "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/include/axl-sdk/axl" >"$WORK/stage.diff" 2>&1
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
    # 2. libstdc++.a carries the hook definitions (INVERTED AT P4).
    # ---------------------------------------------------------------
    # Read off the TOOLCHAIN's archive, located the same way axl-cc locates it
    # so an AXL_*_GXX override moves both together. Before P4 this asserted
    # libaxl-cxx.a; the symbols are the same, the provider is the opposite.
    # The g++ comes from axl-toolchains.conf, resolved the way that file
    # documents (`${AXL_<A>_GXX:-$AXL_<A>_GXX_DEFAULT}`) -- it is deliberately
    # both valid sh and valid make so callers need not hard-code a path. A
    # literal /opt path here would go stale the first time the pin moves, and
    # would test a DIFFERENT libstdc++ from the one axl-cc links.
    local nm_bin="nm" gxx_bin
    [[ "$arch" == "AARCH64" ]] && nm_bin="aarch64-linux-gnu-nm"
    # shellcheck disable=SC1091
    source "$PROJECT_DIR/scripts/axl-toolchains.conf"
    if [[ "$cc_arch" == "aa64" ]]; then
        gxx_bin="${AXL_AA64_GXX:-$AXL_AA64_GXX_DEFAULT}"
    else
        gxx_bin="${AXL_X64_GXX:-$AXL_X64_GXX_DEFAULT}"
    fi
    local stdcxx_a=""
    [[ -x "$gxx_bin" ]] && stdcxx_a="$("$gxx_bin" -print-file-name=libstdc++.a 2>/dev/null)"

    [[ -n "$stdcxx_a" && -f "$stdcxx_a" ]]
    check "$?" "$arch: located the toolchain's libstdc++.a (${stdcxx_a:-<nothing>})"

    if [[ -f "$stdcxx_a" ]]; then
        "$nm_bin" --defined-only "$stdcxx_a" >"$WORK/cxxlib.nm" 2>/dev/null
        local sym missing=""
        for sym in "${THROW_SYMS[@]}" "${REHASH_SYMS[@]}" "${TREE_SYMS[@]}"; do
            grep -q " $sym\$" "$WORK/cxxlib.nm" || missing="$missing $sym"
        done
        [[ -z "$missing" ]]
        check "$?" "$arch: libstdc++.a defines all $((${#THROW_SYMS[@]} + ${#REHASH_SYMS[@]} + ${#TREE_SYMS[@]})) hook symbols${missing:+ (missing:$missing)}"

        # The mangled names above must still BE these functions. Without this a
        # renamed mangling turns every grep into a silent no-match.
        #
        # Via a file, not a pipe: `set -o pipefail` is on, and `grep -q` exits
        # on the first match, SIGPIPEing nm -- so `nm | grep -q` reports
        # FAILURE on the runs where it found what it was looking for.
        "$nm_bin" -C --defined-only "$stdcxx_a" >"$WORK/cxxlib.dm" 2>/dev/null
        grep -qF "std::__throw_out_of_range_fmt(char const*, ...)" "$WORK/cxxlib.dm"
        check "$?" "$arch: the mangled names still demangle to the std:: functions"
    fi

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

    # Every traced symbol must be DEFINED BY libstdc++.a. ld prints
    # "<file>: definition of <sym>" for the archive member that supplied it.
    # INVERTED AT P4: the provider used to be libaxl-cxx.a. Still asserted
    # positively rather than as "not us" -- an absence would also be satisfied
    # by a symbol nothing defined at all, which --no-undefined would catch but
    # this check would report as a pass.
    local from_stdcxx=1
    for sym in "${REHASH_SYMS[@]}" _ZSt20__throw_length_errorPKc; do
        if ! grep -E "libstdc\+\+\.a.*: definition of $sym\$" "$WORK/build.log" >/dev/null; then
            from_stdcxx=0
            echo "      $sym came from: $(grep -E ": definition of $sym\$" "$WORK/build.log" | head -1 || echo '<nothing>')"
        fi
    done
    [[ "$from_stdcxx" -eq 1 ]]
    check "$?" "$arch: libstdc++.a defined the hook symbols (P4 inversion)"

    # ---------------------------------------------------------------
    # 3b. libstdc++.a IS linked, and libaxl-cxx.a is gone (P4).
    # ---------------------------------------------------------------
    # The §8 constraint is unchanged and is NOT what this measures: the SDK
    # still redistributes no libstdc++, because axl-cc names the consumer's
    # installed copy via `-print-file-name`. What regressing here would mean
    # now is a link that quietly went back to hand-written substitutes, i.e.
    # a stale staged SDK making every case below measure the previous build.
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

    grep -q "libstdc++\.a" "$WORK/tracelink.log"
    check "$?" "$arch: the C++ link opens libstdc++.a"

    ! grep -q "libaxl-cxx\.a" "$WORK/tracelink.log"
    check "$?" "$arch: libaxl-cxx.a is gone from the link (P4)"

    # Positive control: -t must actually have produced a listing, or the
    # ABSENCE above proves nothing. libaxl.a is the archive that is on every
    # link whatever happens to the C++ side, so it is the right control now
    # that libaxl-cxx.a is the thing being asserted absent.
    grep -q "libaxl\.a" "$WORK/tracelink.log"
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
    # A consumer writing plain C++ hits these without opting into anything.
    # Two of them had no definition at all until this fixture existed, and
    # both failed only at LINK: `new (std::nothrow) T` needs the std::nothrow
    # OBJECT and an over-aligned `new` calls a different operator entirely.
    # Both come from libsupc++ since P4 -- which firmware DOES link now, and
    # that is the change this case silently depends on.
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
    # 5c. -frtti links with no mode flag and needs no extra library.
    # ---------------------------------------------------------------
    # libsupc++ carries the __cxxabiv1 type_info vtables and __dynamic_cast.
    # RTTI used to be the ONE thing that put libstdc++.a on the link line, and
    # this pair asserted exactly that asymmetry; P4 puts it there for every
    # C++ link, so the asymmetry is gone and what is left to pin is that
    # -frtti needs no separate opt-in plumbing.
    #
    # Still worth running rather than deleting: -frtti is the workload that
    # first SPLIT the aa64 relocation table (see the run assertion below), and
    # it remains the only fixture that exercises type_info at all.
    "$AXL_CXX" --arch "$cc_arch" --release -frtti \
        "$RTTI_SRC" -o "$WORK/rtti-$cc_arch.efi" >"$WORK/rtti.log" 2>&1
    check "$?" "$arch: -frtti links with no mode flag"
    [[ -s "$WORK/rtti.log" ]] && grep -oP "undefined reference to .\K[^']+" \
        "$WORK/rtti.log" | head -3 | sed 's/^/      /'

    # Read off the LINK LINE rather than inferred from success: --verbose
    # prints the ld invocation, and libstdc++.a must appear on BOTH the -frtti
    # link and the plain one since P4.
    #
    # Each half still needs its exit status checked first. A presence proves
    # nothing if the string came from an error message rather than a command
    # line -- axl-cc's old "-frtti needs libstdc++.a" diagnostic contained the
    # literal string, so this grep would once have passed on the exact failure
    # it is meant to catch.
    "$AXL_CXX" --arch "$cc_arch" --release --verbose -frtti \
        "$RTTI_SRC" -o "$WORK/rtti-v.efi" >"$WORK/rtti-v.log" 2>&1
    rc=$?
    [[ "$rc" -eq 0 && -f "$WORK/rtti-v.efi" ]] \
        && grep -q -- "libstdc++\.a" "$WORK/rtti-v.log" \
        && grep -q "libaxl\.a" "$WORK/rtti-v.log"
    check "$?" "$arch: -frtti names libstdc++.a on a link that succeeded (rc=$rc)"

    "$AXL_CXX" --arch "$cc_arch" --release --verbose \
        "$SRC" -o "$WORK/nortti-v.efi" >"$WORK/nortti-v.log" 2>&1
    rc=$?
    # INVERTED AT P4. The control is libaxl.a, which is on every link whatever
    # the C++ side does, so a --verbose that printed nothing cannot read as a
    # pass.
    [[ "$rc" -eq 0 && -f "$WORK/nortti-v.efi" ]] \
        && grep -q "libaxl\.a" "$WORK/nortti-v.log" \
        && grep -q "libstdc++\.a" "$WORK/nortti-v.log"
    check "$?" "$arch: a DEFAULT C++ link also names libstdc++.a (rc=$rc)"

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
    # 5f. axl::unique_handle owns a REAL object and gives the memory back.
    # ---------------------------------------------------------------
    # `make check-handle-exclusions` covers the compile-time contract —
    # which types get a handle, which are refused, and what the refusal
    # says. What it cannot see is whether the deleter reaches the destroy
    # function the header named, so this runs the four cases AXL_AUTOPTR
    # cannot express (scope, move, class member, factory return) against
    # live allocation counts.
    #
    # Each case asserts `alive=1` BEFORE `freed=1`. Without that half,
    # `freed=1` would hold trivially on a build with no allocation
    # accounting — including this one, which is --release — and the whole
    # fixture would be unable to fail. The counter has to be seen moving.
    "$AXL_CXX" --arch "$cc_arch" --release "$HANDLE_SRC" \
        -o "$WORK/handle-$cc_arch.efi" >"$WORK/handle.log" 2>&1
    check "$?" "$arch: the unique_handle fixture builds"
    if [[ -f "$WORK/handle-$cc_arch.efi" ]]; then
        timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
            --arch "$arch" --timeout 45 "$WORK/handle-$cc_arch.efi" \
            >"$WORK/handlerun.log" 2>&1
        tr -d '\r' < "$WORK/handlerun.log" > "$WORK/handlerun.clean"
        local hline
        for hline in 'handle: scope alive=1'      'handle: scope freed=1' \
                     'handle: move alive=1'       'handle: move src-empty=1' \
                     'handle: move freed=1' \
                     'handle: member alive=1'     'handle: member usable=1' \
                     'handle: member freed=1' \
                     'handle: release disarmed=1' 'handle: release freed=1' \
                     'handle: empty null=1'       'handle: done'; do
            grep -Fxq "$hline" "$WORK/handlerun.clean"
            check "$?" "$arch: unique_handle — $hline"
        done
    fi

    # ---------------------------------------------------------------
    # 6. The two halt paths: reached, named, and NOT returned from.
    # ---------------------------------------------------------------
    halt_fixture "$arch" "$cc_arch" out-of-range "$THROW_SRC" \
        "cxx-throw: about to call vector::at(99) on a 3-element vector" \
        "terminate: uncaught exception of type St12out_of_range" \
        "  what(): vector::_M_range_check" \
        "cxx-throw: UNREACHABLE"
    halt_fixture "$arch" "$cc_arch" bad-alloc "$BADALLOC_SRC" \
        "cxx-badalloc: about to allocate more than any heap can serve" \
        "terminate: uncaught exception of type St9bad_alloc" \
        "  what(): std::bad_alloc" \
        "cxx-badalloc: UNREACHABLE"
}

# Build a fixture that is SUPPOSED to halt, run it, and assert four things
# that are easy to conflate: it got as far as the call, the runtime NAMED what
# went wrong, it said what() rather than only the type, and it did not RETURN.
# The last is the one that matters most -- gcc compiles the code after a throw
# site assuming it is unreachable, so a path that returns resumes in a state
# nothing modelled.
#
# WHAT PRODUCES THE MESSAGE CHANGED AT P4, and the assertions moved with it.
# These used to reach AXL's own std::__throw_* stubs in libaxl-cxx.a, which
# printed "axl-cxxabi: __throw_out_of_range_fmt" and halted. They now reach
# libstdc++'s real ones, which THROW; with no handler in a -fno-exceptions
# consumer the unwinder calls std::terminate, and AXL's replacement handler
# (src/cxxrt/axl-cxxrt-terminate.cpp) prints the type and what(). That is
# strictly better diagnostics -- "vector::_M_range_check: __n (which is 99)"
# instead of a function name -- and it is why every C++ link takes the
# exceptions linker script now: without a registered frame table the throw
# would reach terminate through _URC_FATAL_PHASE1_ERROR and print neither.
#
# The TYPE line is matched exactly (grep -Fxq) because it is AXL's own output.
# The what() text is matched as a PREFIX: its tail is libstdc++'s wording, and
# pinning a third-party string exactly would fail on a toolchain bump without
# anything being wrong.
halt_fixture() {
    local arch="$1" cc_arch="$2" name="$3" src="$4"
    local reached="$5" type_line="$6" what_pat="$7" unreachable="$8"
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
    grep -Fxq "$type_line" "$WORK/$name-run.clean"
    check "$?" "$arch: $name named the exception type instead of dying mutely"
    grep -qF "$what_pat" "$WORK/$name-run.clean"
    check "$?" "$arch: $name recovered what() ($what_pat...)"
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
