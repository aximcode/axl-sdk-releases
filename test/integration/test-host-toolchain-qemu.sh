#!/bin/bash
# test-meta: arch=x64 needs= est=180 local-only=0
# test-host-toolchain-qemu.sh — AXL_TOOLCHAIN=host builds and boots x64 with
# the HOST compiler and no bare-metal toolchain involvement.
#
# http-server.c is not optional here. Simple sources link without any libc
# because --gc-sections drops the vendored code (libvterm, lzma, mbedTLS) that
# references the plain leaf names; http-server is the shipped source that keeps
# them, and it is the case that exposed "no libc is needed at all" as wrong.
# See docs/AXL-Host-Toolchain-Design.md §5.3.
#
# `host` is EXPLICIT in the first half. On a box with the bare-metal toolchain
# installed an unset AXL_TOOLCHAIN resolves to `axl`, so leaving it unset there
# would exercise the wrong path and prove nothing about `host`.
#
# The second half is the opposite assertion: `auto` -- the x64 DEFAULT -- picks
# between the two by probing the compiler, and must keep choosing `axl`
# wherever the bare-metal toolchain is installed. Both directions are asserted,
# because a probe stuck on either answer is invisible from the other side.
#
# Requires a staged SDK (scripts/install.sh --arch x64); point AXL_SDK_PREFIX
# at it, or take the default from sdk-prefix.sh. Exits 2 if it is absent --
# "the SDK is not staged" and "the feature is broken" are opposite facts and
# must not share an exit code.

source "$(dirname "$0")/common-test.sh"
# Every assertion below reads a return code from a command that is EXPECTED to
# fail in half the cases, so errexit would abort the run at the first refusal
# it is asserting on.
set +e
TEST_SKIP_RATCHET=1

SDK="$(test_sdk_dir)"
AXL_CC="$SDK/bin/axl-cc"
AXL_CXX="$SDK/bin/axl-c++"
TC_CONF="$SDK/share/axl/axl-toolchains.conf"
if [[ ! -x "$AXL_CC" || ! -x "$AXL_CXX" || ! -d "$SDK/lib/axl/x64" ]]; then
    echo "ERROR: staged SDK missing at $SDK — run 'scripts/install.sh --arch x64' first" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Per-file, as every host-side test in this tree defines it; the tally itself
# comes from common-test.sh so the summary line is the shared one. NOT
# test_count_results -- that parses a QEMU serial log for assertions that ran
# inside the guest, and all but one of these run here.
check() {  # check <rc:0=pass> <msg>
    if [[ "$1" == "0" ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
}

echo "=== AXL_TOOLCHAIN=host, x64 ==="

# 1. hello builds and links (the boot it produces is assertion 12).
AXL_TOOLCHAIN=host "$AXL_CC" "$PROJECT_DIR/sdk/examples/hello.c" \
    -o "$WORK/hello.efi" > "$WORK/hello.log" 2>&1
hello_rc=$?
check $hello_rc "host: hello.c compiles and links"
[[ $hello_rc -ne 0 ]] && sed 's/^/      /' "$WORK/hello.log" | head -12

# 2. http-server builds -- THE assertion that discriminates. Without
#    libaxl-standin.a on the line this fails with `undefined reference to
#    memcpy` and five siblings, while hello.c still links.
AXL_TOOLCHAIN=host "$AXL_CC" \
    "$PROJECT_DIR/sdk/examples/http-server.c" \
    -o "$WORK/http.efi" > "$WORK/http.log" 2>&1
http_rc=$?
check $http_rc "host: http-server.c links (the vendored leaf names resolve)"
[[ $http_rc -ne 0 ]] && sed 's/^/      /' "$WORK/http.log" | head -12

# 3. Nothing from the bare-metal toolchain is INVOKED.
#
#    Read off the command lines --verbose echoes, not off `strings` on the
#    image: every .efi is stripped, so a DEFAULT (`axl`) build carries zero
#    occurrences of the toolchain path too. Measured -- a strings-based
#    assertion here passes under both variants and discriminates nothing.
#
#    3a is the control. A detector's silence is worth nothing until it has
#    been shown it can speak, and the same grep over the same kind of log must
#    FIND the toolchain when the default variant built it.
#
#    BOTH HALVES SKIP TOGETHER on a machine with no bare-metal toolchain --
#    which is precisely the machine `host` mode exists to serve, so failing
#    there would tell that user their install is broken when it is working as
#    designed. And they skip TOGETHER rather than 3b surviving alone: with no
#    toolchain, "no command line names it" is trivially true and proves
#    nothing without its control. Two assertions either way, so the count does
#    not drift with the machine (feedback_balancer_count).
#
#    Presence is the COMPILER, not the directory (§4.1): a partial extract
#    leaves the directory in place and nothing in it.
# shellcheck disable=SC1090
. "$TC_CONF"
TC_DIR="${AXL_X64_TOOLCHAIN_DIR:-}"
TC_GCC="${AXL_X64_GCC_DEFAULT:-}"
# One derivation, read by this pair AND by the `auto` pair below, so the two
# cannot disagree about whether this machine has a bare-metal toolchain.
HAVE_BAREMETAL=false
[[ -n "$TC_DIR" && -n "$TC_GCC" && -x "$TC_GCC" ]] && HAVE_BAREMETAL=true

# Unconditional: assertion 7 reads this log, and it is a host-mode build that
# owes nothing to the bare-metal toolchain.
AXL_TOOLCHAIN=host "$AXL_CC" --verbose "$PROJECT_DIR/sdk/examples/hello.c" \
    -o "$WORK/hostv.efi" > "$WORK/host-verbose.log" 2>&1
hostv_rc=$?

if [[ "$HAVE_BAREMETAL" == "true" ]]; then
    "$AXL_CC" --verbose "$PROJECT_DIR/sdk/examples/hello.c" \
        -o "$WORK/ctl.efi" > "$WORK/ctl-verbose.log" 2>&1
    ctl_rc=$?
    if [[ $ctl_rc -eq 0 ]] && grep -qF "$TC_DIR" "$WORK/ctl-verbose.log"; then
        check 0 "control: the default variant DOES invoke $TC_DIR"
    else
        check 1 "control: the default variant DOES invoke $TC_DIR (rc=$ctl_rc)"
    fi

    if [[ $hostv_rc -eq 0 ]] && ! grep -qF "$TC_DIR" "$WORK/host-verbose.log"; then
        check 0 "host: no command line names the bare-metal toolchain"
    else
        check 1 "host: no command line names the bare-metal toolchain (rc=$hostv_rc)"
        grep -nF "$TC_DIR" "$WORK/host-verbose.log" | head -3 | sed 's/^/      /'
    fi
else
    check 0 "SKIP: no bare-metal x64 toolchain — control not run"
    check 0 "SKIP: no bare-metal x64 toolchain — nothing to prove absent from the host build"
fi

# 4-5. The freestanding include set comes from the COMPILER's own directory,
#      and /usr/include is absent from it. Read out of the depfile the REAL
#      compile writes, so this pins where headers actually resolved rather
#      than which flags were passed.
#      Resolve the include dir the way axl-cc's `host` arm resolves the
#      COMPILER -- AXL_X64_HOST_GCC first, then PATH. Asking bare `gcc` reads a
#      different compiler than the driver used whenever that override is set,
#      and an empty answer would silently degrade the grep below to a match on
#      "/stdint.h", which every depfile satisfies.
HOST_GCC="${AXL_X64_HOST_GCC:-$(command -v gcc || true)}"
HOST_INC=""
[[ -n "$HOST_GCC" ]] && \
    HOST_INC="$("$HOST_GCC" -print-file-name=include 2>/dev/null || true)"
HOST_INC="${HOST_INC%/}"
printf '#include <stdint.h>\nuint32_t axl_test_dep(void) { return 1; }\n' \
    > "$WORK/dep.c"
AXL_TOOLCHAIN=host "$AXL_CC" -c "$WORK/dep.c" -o "$WORK/dep.o" \
    -MD -MF "$WORK/dep.d" > "$WORK/dep.log" 2>&1
dep_rc=$?
if [[ -z "$HOST_INC" || ! -d "$HOST_INC" ]]; then
    check 1 "host: <stdint.h> resolves inside the compiler's own include dir (could not resolve it: '${HOST_GCC:-<no gcc>}' gave '${HOST_INC:-<nothing>}')"
elif [[ $dep_rc -eq 0 && -f "$WORK/dep.d" ]] \
     && grep -qF "$HOST_INC/stdint.h" "$WORK/dep.d"; then
    check 0 "host: <stdint.h> resolves inside $HOST_INC"
else
    check 1 "host: <stdint.h> resolves inside $HOST_INC (rc=$dep_rc)"
    sed 's/^/      /' "$WORK/dep.log" | head -8
fi
if [[ -f "$WORK/dep.d" ]] && ! grep -q '/usr/include/' "$WORK/dep.d"; then
    check 0 "host: nothing resolved under /usr/include"
else
    check 1 "host: nothing resolved under /usr/include"
    grep -o '/usr/include/[^ ]*' "$WORK/dep.d" 2>/dev/null | head -3 | sed 's/^/      /'
fi

# 6. THE HERMETICITY EXEMPTION IS REPLACED, NOT EXTENDED (§5.2). The existing
#    rule exempts the compiler's GRANDPARENT so a /usr/local toolchain is not
#    reported as a host reach; for /usr/bin/gcc that grandparent is /usr, and
#    carrying it over would whitelist the entire host tree.
#
#    An ABSOLUTE #include is the probe, because -nostdinc cannot stop one --
#    which is precisely why the depfile half of the check exists. The header
#    must be self-contained: glibc's <stdio.h> pulls <bits/...> through the
#    search path that -nostdinc just emptied, so the compile would fail for
#    the wrong reason and prove nothing about the exemption.
HOST_HDR=""
for _c in /usr/include/stdc-predef.h /usr/include/sysexits.h; do
    if [[ -f "$_c" ]] && ! grep -q '^[[:space:]]*#[[:space:]]*include' "$_c"; then
        HOST_HDR="$_c"; break
    fi
done
if [[ -z "$HOST_HDR" ]]; then
    for _c in /usr/include/*.h; do
        if [[ -f "$_c" ]] && ! grep -q '^[[:space:]]*#[[:space:]]*include' "$_c"; then
            HOST_HDR="$_c"; break
        fi
    done
fi
if [[ -z "$HOST_HDR" ]]; then
    check 1 "host: a /usr/include header is still refused (no self-contained header found to probe with)"
else
    printf '#include "%s"\nint main(void) { return 0; }\n' "$HOST_HDR" \
        > "$WORK/reach.c"
    AXL_TOOLCHAIN=host "$AXL_CC" "$WORK/reach.c" -o "$WORK/reach.efi" \
        > "$WORK/reach.log" 2>&1
    reach_rc=$?
    # THE MESSAGE, not just the path. A compile that failed for an unrelated
    # reason names $HOST_HDR too -- it is on the #include line -- so the path
    # alone cannot tell "the hermeticity check fired" from "the compile broke".
    # The banner identifies which check refused; the path then says it saw the
    # right file.
    if [[ $reach_rc -ne 0 ]] \
       && grep -q "reached the HOST's headers" "$WORK/reach.log" \
       && grep -qF "$HOST_HDR" "$WORK/reach.log"; then
        check 0 "host: an absolute include of $HOST_HDR is still refused by name"
    else
        check 1 "host: an absolute include of $HOST_HDR is still refused by name (rc=$reach_rc)"
        sed 's/^/      /' "$WORK/reach.log" | head -8
    fi
fi

# 7. The hermeticity -M pass runs with the SAME include flags as the compile
#    it is checking. A pass that resolves headers from a different search path
#    answers a different question -- that block's own comment says so.
#
#    Read off the echoed command rather than off a build outcome, and this is
#    deliberate rather than lazy: -nostdinc makes the REAL compile strictly
#    more restrictive, so every /usr path the unflagged -M pass could see is
#    one the compile would already have failed on. The divergence it actually
#    prevents needs a /usr/local/include shadowing the compiler's own headers,
#    which this test must not create on the developer's box.
if grep -E '^\+ .*-M .*-MF ' "$WORK/host-verbose.log" > "$WORK/herm-cmd.log" \
   && grep -q -- '-nostdinc' "$WORK/herm-cmd.log"; then
    check 0 "host: the hermeticity -M pass carries -nostdinc"
else
    check 1 "host: the hermeticity -M pass carries -nostdinc"
    sed 's/^/      /' "$WORK/herm-cmd.log" | head -3
fi

echo "=== the refusals ==="

# 8-9. aa64 refuses host, by name. This fires before the "no SDK libraries for
#      arch aa64" and "compiler not found" checks, so it is the same answer on
#      a one-arch stage as on a full one -- which is also why a bare `rc != 0`
#      would not discriminate here: those two refusals are non-zero too. Both
#      halves therefore read the message.
AXL_TOOLCHAIN=host "$AXL_CC" --arch aa64 \
    "$PROJECT_DIR/sdk/examples/hello.c" -o "$WORK/no.efi" \
    > "$WORK/aa64.log" 2>&1
aa64_rc=$?
if [[ $aa64_rc -ne 0 ]] && grep -q 'AXL_TOOLCHAIN=host is x64-only' "$WORK/aa64.log"; then
    check 0 "host: aa64 refuses the host variant"
else
    check 1 "host: aa64 refuses the host variant (rc=$aa64_rc)"
    sed 's/^/      /' "$WORK/aa64.log" | head -6
fi
grep -q 'pei-aarch64-little' "$WORK/aa64.log"
check $? "host: the aa64 refusal names pei-aarch64-little as the reason"

# 10-11. C++ refuses, and names the remedy (§6.3) -- a refusal that does not
#        say what to do next is half an answer.
#        Same reasoning as the aa64 pair: "no x64 C++ compiler is configured"
#        is also a non-zero exit, and is exactly the wrong answer this refusal
#        exists to pre-empt -- so the first half reads the message too.
printf 'int main() { return 0; }\n' > "$WORK/t.cpp"
AXL_TOOLCHAIN=host "$AXL_CXX" "$WORK/t.cpp" -o "$WORK/t.efi" \
    > "$WORK/cxx.log" 2>&1
cxx_rc=$?
if [[ $cxx_rc -ne 0 ]] \
   && grep -q 'C++ is not available under the host compiler' "$WORK/cxx.log"; then
    check 0 "host: C++ is refused"
else
    check 1 "host: C++ is refused (rc=$cxx_rc)"
    sed 's/^/      /' "$WORK/cxx.log" | head -6
fi
grep -q 'axl toolchain install x64' "$WORK/cxx.log"
check $? "host: the C++ refusal names the remedy verb"

echo "=== and it boots ==="

# 12. The image the host compiler produced actually runs under OVMF. Every
#     assertion above is about the build; this is the one that says the bytes
#     are a working UEFI image. hello.c takes its greeting from argv.
"$PROJECT_DIR/scripts/run-qemu.sh" --arch X64 --timeout 45 \
    "$WORK/hello.efi" host > "$WORK/boot.log" 2>&1
if grep -qF 'Hello, host!' "$WORK/boot.log"; then
    check 0 "host: the produced image boots on OVMF and prints its greeting"
else
    check 1 "host: the produced image boots on OVMF and prints its greeting"
    tail -25 "$WORK/boot.log" | sed 's/^/      /'
fi

echo "=== AXL_TOOLCHAIN=auto (the x64 default) ==="

# The fall-back build, run FIRST because three assertions read its log.
#
# The toolchain is hidden by pointing the LOCATOR at a path that does not
# exist. Never by moving or renaming /opt -- a run that died between the move
# and the restore would leave this box with no toolchain at all, and that
# damage is not recoverable by git.
#
# Set PER INVOCATION and never exported: an exported override would leak into
# every assertion after it and silently hide the toolchain from those too,
# including the control that has to see it.
AXL_X64_GCC=/nonexistent/x86_64-elf-gcc \
    "$AXL_CC" --verbose "$PROJECT_DIR/sdk/examples/hello.c" \
    -o "$WORK/auto-b.efi" > "$WORK/auto-absent.log" 2>&1
absent_rc=$?

# 13-14. WITH the bare-metal toolchain installed, the default must resolve to
#        `axl` -- the direction that is invisible from the fall-back side. A
#        probe stuck on "absent" would move every consumer with a toolchain
#        onto host gcc, silently, and every other assertion here would still
#        pass (§7.4).
#
#        14 is the other half of that: the fall-back must not keep reaching
#        into the toolchain it just decided is missing. Hiding only $GCC_BIN
#        leaves this box's bare-metal ld/objcopy present and named by the
#        conf's binutils prefix, so a fall-back that forgot to clear $CROSS
#        would build fine HERE and fail on the machine the variant exists for.
#
#        Both SKIP together where there is no bare-metal toolchain: there the
#        default correctly resolves to host, so there is no `axl` resolution to
#        assert and nothing whose absence would mean anything. Two assertions
#        either way (feedback_balancer_count).
if [[ "$HAVE_BAREMETAL" == "true" ]]; then
    "$AXL_CC" --verbose "$PROJECT_DIR/sdk/examples/hello.c" \
        -o "$WORK/auto-a.efi" > "$WORK/auto-present.log" 2>&1
    present_rc=$?
    # `toolchain=axl (auto)`, NOT `toolchain=axl`. The suffix is the only
    # thing that says auto was EXERCISED rather than merely agreed with:
    # spec 3.1 tells CI to export AXL_TOOLCHAIN=axl, and in that environment
    # the bare grep passes while this file silently stops testing the feature
    # it exists for. Assertion 15 pins the same distinction on the other side.
    if [[ $present_rc -eq 0 ]] && grep -qF 'toolchain=axl (auto)' "$WORK/auto-present.log"; then
        check 0 "auto: resolves to axl when the bare-metal toolchain is installed"
    else
        check 1 "auto: resolves to axl when the bare-metal toolchain is installed (rc=$present_rc)"
        grep -m1 '^\[axl-cc\] ' "$WORK/auto-present.log" | sed 's/^/      /'
    fi

    if [[ $absent_rc -eq 0 ]] && ! grep -qF "$TC_DIR" "$WORK/auto-absent.log"; then
        check 0 "auto: the host fall-back invokes nothing from $TC_DIR"
    else
        check 1 "auto: the host fall-back invokes nothing from $TC_DIR (rc=$absent_rc)"
        grep -nF "$TC_DIR" "$WORK/auto-absent.log" | head -3 | sed 's/^/      /'
    fi
else
    check 0 "SKIP: no bare-metal x64 toolchain — auto cannot resolve to axl here"
    check 0 "SKIP: no bare-metal x64 toolchain — nothing for the fall-back to leak"
fi

# 15-16. WITHOUT it, the default falls back to host AND still produces an
#        image. The banner is read rather than the exit status because a build
#        that succeeded proves only that SOME compiler ran -- and the
#        ` (auto)` suffix rather than the bare word, so an exported
#        AXL_TOOLCHAIN=host would fail this instead of satisfying it.
if [[ $absent_rc -eq 0 ]] && grep -qF 'toolchain=host (auto)' "$WORK/auto-absent.log"; then
    check 0 "auto: falls back to host when the bare-metal toolchain is absent"
else
    check 1 "auto: falls back to host when the bare-metal toolchain is absent (rc=$absent_rc)"
    sed 's/^/      /' "$WORK/auto-absent.log" | head -12
fi
[[ -f "$WORK/auto-b.efi" ]]
check $? "auto: the fall-back actually produces an image"

# 17-18. THE CONTROL. An EXPLICIT AXL_TOOLCHAIN=axl with the toolchain hidden
#        must keep today's hard failure, naming the compiler path it looked
#        for. Without this, `auto` could mask a genuinely broken bare-metal
#        install by quietly building with host gcc and nobody would find out --
#        and it is that guard, not a pin file, that lets CI express "always
#        bare-metal" in its environment (§3.1, §7.4).
#
#        rc is captured on the very next line. `echo "rc=$?"` first would
#        report the ECHO's status and read as a pass.
AXL_TOOLCHAIN=axl AXL_X64_GCC=/nonexistent/x86_64-elf-gcc \
    "$AXL_CC" "$PROJECT_DIR/sdk/examples/hello.c" -o "$WORK/auto-c.efi" \
    > "$WORK/explicit-axl.log" 2>&1
explicit_rc=$?
# The image too: a fall-back would exit 0 AND leave one behind, so asserting
# both says "it refused" rather than only "it complained".
[[ $explicit_rc -ne 0 && ! -f "$WORK/auto-c.efi" ]]
check $? "auto: explicit AXL_TOOLCHAIN=axl does NOT fall back to host"
grep -qF '/nonexistent/x86_64-elf-gcc' "$WORK/explicit-axl.log"
check $? "auto: the explicit-axl failure names the compiler it looked for"

echo "=== the missing-hosted-header diagnostic ==="

# 19-22. A hosted header under host fails WITH the explanation (§6.3): "fatal
#        error: string.h: No such file or directory" does not say "this mode
#        has no C library" -- the failure a host-mode user is most likely to
#        hit and least likely to diagnose. 20-21 pin that this is ADDITIVE:
#        the compiler's own message (the only part naming the file and line)
#        must survive next to the new explanation, not be replaced by it.
printf '#include <string.h>\nint main(void){return 0;}\n' > "$WORK/h.c"
AXL_TOOLCHAIN=host "$AXL_CC" "$WORK/h.c" -o "$WORK/h.efi" \
    > "$WORK/hosted.log" 2>&1
hosted_rc=$?
if [[ $hosted_rc -ne 0 ]]; then
    check 0 "host: a hosted header fails the build"
else
    check 1 "host: a hosted header fails the build"
fi
grep -q 'string.h' "$WORK/hosted.log"
check $? "host: the compiler's own error is preserved, not replaced"
grep -q 'has no C standard library' "$WORK/hosted.log"
check $? "host: the failure explains that host mode is freestanding"
grep -q 'axl toolchain install x64' "$WORK/hosted.log"
check $? "host: the hosted-header failure names the remedy"

# 23-24. limits.h is the C-standard freestanding exception that ISN'T
#        available under host (measured, §5.1): gcc's own limits.h reaches
#        the platform's via #include_next, and -nostdinc removes it, so it
#        must be caught by the same note -- and the "still available" list
#        the note prints must NOT include it, because a message naming an
#        unavailable header as available is worse than none. Pinned as the
#        exact two lines that make up that list (not a substring grep):
#        limits.h landing anywhere in either line is exactly the regression
#        this guards against.
printf '#include <limits.h>\nint main(void){return 0;}\n' > "$WORK/lim.c"
AXL_TOOLCHAIN=host "$AXL_CC" "$WORK/lim.c" -o "$WORK/lim.efi" \
    > "$WORK/limits.log" 2>&1
limits_rc=$?
if [[ $limits_rc -ne 0 ]] && grep -q 'has no C standard library' "$WORK/limits.log"; then
    check 0 "host: limits.h is caught by the same note as the other hosted headers"
else
    check 1 "host: limits.h is caught by the same note as the other hosted headers (rc=$limits_rc)"
    sed 's/^/      /' "$WORK/limits.log" | head -12
fi
# The WHOLE parenthetical, reassembled, not just two anchor lines: pinning
# only the first and last line would let a THIRD line inserted between them
# evade this check entirely. Collect from "FREESTANDING headers (" through
# the first line that CLOSES the parenthetical (ends in ").", wherever that
# actually falls), stripping the leading indent so line-wrapping is not
# part of what is being pinned.
avail_block="$(awk '
    /FREESTANDING headers \(/ { grab=1 }
    grab {
        line=$0; sub(/^ */, "", line)
        printf "%s ", line
        if (line ~ /\)\.$/) exit
    }
' "$WORK/limits.log" | sed 's/ *$//')"
if [[ "$avail_block" == 'FREESTANDING headers (stddef.h, stdarg.h, stdint.h, stdbool.h, float.h, iso646.h, stdalign.h, stdatomic.h, stdnoreturn.h).' ]]; then
    check 0 "host: limits.h is absent from the 'still available' freestanding list"
else
    check 1 "host: limits.h is absent from the 'still available' freestanding list"
    printf '      got: [%s]\n' "$avail_block"
fi

echo "=== the no-compiler-anywhere diagnostic ==="

# 25-28. Routed from Task 3's review, confirmed by two reviewers: on a
#        machine with NEITHER the bare-metal toolchain NOR a host gcc, `auto`
#        falls back to host and GCC_BIN ends up empty. That used to print
#        "axl-toolchains.conf was not found ... the install may be
#        incomplete" -- FALSE, the conf was read fine; there is simply no
#        compiler anywhere. `auto` makes this the DEFAULT outcome on exactly
#        the toolchain-free machine this feature serves, so it is now the
#        first thing such a user sees.
#
#        This state cannot be created by deleting this box's real gcc --
#        pointing AXL_X64_HOST_GCC at a nonexistent path does NOT reproduce
#        it: GCC_BIN would then be that (non-empty) bad path, which already
#        hits the correct "compiler not found at $GCC_BIN" branch, not the
#        false one. The empty-GCC_BIN branch fires only when NEITHER an
#        override NOR `command -v gcc` resolves anything, so gcc is shadowed
#        from PATH for exactly this one subprocess instead: every OTHER tool
#        axl-cc needs before this check runs (readlink, dirname, basename,
#        mkdir, plus grep/sed/cat/rm for the harness itself) is symlinked
#        into a private directory that has no gcc in it.
NOGCC_BIN="$WORK/nogcc-bin"
mkdir -p "$NOGCC_BIN"
for _t in readlink dirname basename mkdir cat sed grep rm; do
    _p="$(command -v "$_t" 2>/dev/null || true)"
    [[ -n "$_p" ]] && ln -sf "$_p" "$NOGCC_BIN/$_t"
done
printf 'int main(void){return 0;}\n' > "$WORK/nc.c"
# AXL_X64_HOST_GCC explicit and empty, not merely left ambient: an
# inherited value from the environment this suite runs in would route
# GCC_BIN down a DIFFERENT branch (a set-but-broken locator, not the
# empty one this scenario needs) and fail this block for the wrong
# reason rather than falsely pass it -- still worth pinning explicitly.
AXL_X64_HOST_GCC='' PATH="$NOGCC_BIN" AXL_TOOLCHAIN=auto \
    AXL_X64_GCC=/nonexistent/x86_64-elf-gcc \
    "$AXL_CC" "$WORK/nc.c" -o "$WORK/nogcc.efi" > "$WORK/nogcc.log" 2>&1
nogcc_rc=$?
# THE SANDBOX'S OWN sufficiency, asserted rather than assumed: NOGCC_BIN
# above symlinks only the tools this exact code path needs TODAY
# (verified empirically). A future early call to a tool it lacks would
# otherwise fail the assertions below for a confusing, unrelated reason
# -- bash's own "command not found" (exit 127) is the failure mode
# hardest to diagnose from output alone, so it is checked for by name.
if grep -q ': command not found' "$WORK/nogcc.log"; then
    check 1 "auto: NOGCC_BIN provides every tool axl-cc needs before this check"
else
    check 0 "auto: NOGCC_BIN provides every tool axl-cc needs before this check"
fi
if [[ $nogcc_rc -ne 0 ]]; then
    check 0 "auto: with no compiler anywhere, the build fails"
else
    check 1 "auto: with no compiler anywhere, the build fails"
fi
! grep -q 'axl-toolchains.conf was not found' "$WORK/nogcc.log"
check $? "auto: no compiler anywhere does NOT falsely blame a missing conf"
grep -q 'no x64 C compiler could be found' "$WORK/nogcc.log"
check $? "auto: no compiler anywhere says so plainly"
grep -q 'install a host gcc' "$WORK/nogcc.log" && grep -q 'axl toolchain install x64' "$WORK/nogcc.log"
check $? "auto: no compiler anywhere names BOTH ways forward"

# 29-30. Regression: a host-mode compile failure whose CAUSE is not a hosted
#        header (a plain syntax error) takes axl_cc_note_missing_hosted_header
#        down its OTHER path -- the grep finds nothing. Under this script's
#        `set -euo pipefail`, an unguarded `hit="$(grep ... | sort -u |
#        head -1)"` aborts the WHOLE SCRIPT right there: pipefail turns
#        grep's ordinary "no match" (exit 1) into the pipeline's exit status,
#        and -e kills the shell on that assignment, skipping the caller's
#        `rm -rf "$TMPDIR"` entirely. Measured with `bash -x` before the
#        `|| true` fix: the trace ends at `+ hit=` with no `rm -rf` and no
#        `exit 1` ever executed -- yet the VISIBLE stderr still read "ERROR:
#        compilation failed" with rc=1, indistinguishable from the correct
#        outcome by output alone. That is why this checks the filesystem for
#        the leaked tmpdir instead of the message.
#
#        Scoped to THIS invocation's own PID, not a global
#        /tmp/axl-cc-* glob: run-integration.sh defaults to nproc-2
#        parallel jobs (:139) and other suites invoke axl-cc too, so a
#        concurrent IN-FLIGHT axl-cc's tmpdir would look exactly like a
#        leak to a before/after glob diff -- an intermittent failure in a
#        shared gate, which is worse than no gate at all. `$AXL_CC` is run
#        in the background so `$!` names its own PID before it exits, and
#        axl-cc's TMPDIR is literally "/tmp/axl-cc-$$" (its own PID), so
#        `$!` from here IS that suffix -- confirmed by running this
#        exact background+wait shape against the pre-fix script and
#        finding /tmp/axl-cc-$child_pid left behind.
printf 'int main(void) { this is not valid C; }\n' > "$WORK/synerr.c"
AXL_TOOLCHAIN=host "$AXL_CC" "$WORK/synerr.c" -o "$WORK/synerr.efi" \
    > "$WORK/synerr.log" 2>&1 &
synerr_child=$!
wait "$synerr_child"
synerr_rc=$?
if [[ $synerr_rc -ne 0 && ! -d "/tmp/axl-cc-$synerr_child" ]]; then
    check 0 "host: a non-header compile failure still cleans up its tmpdir"
else
    check 1 "host: a non-header compile failure still cleans up its tmpdir (rc=$synerr_rc)"
    [[ -d "/tmp/axl-cc-$synerr_child" ]] && printf '      leaked: /tmp/axl-cc-%s\n' "$synerr_child"
fi
! grep -q 'NOTE:' "$WORK/synerr.log"
check $? "host: a non-header compile failure prints no spurious hosted-header note"

test_host_summary "host-toolchain"
