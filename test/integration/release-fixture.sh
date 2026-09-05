#!/bin/bash
# release-fixture.sh — build a fake axl-sdk RELEASE on disk, for `--base-url`.
#
# WHY THIS IS SHARED. install.sh's `--base-url` accepts a local directory, so a
# test can fabricate a whole release -- assets, VERSION, SHA256SUMS -- and
# exercise the real installer against it in seconds with no network. Two tests
# now need that: test-installer-assets.sh (does the installer resolve the right
# NAME) and test-install-lifecycle.sh (does a real install survive upgrade,
# downgrade and prune). Keeping a second copy of these builders is the exact
# drift this tree keeps paying for -- two sidecar staging lists, two prune
# lists, a manifest disagreeing with itself -- so there is one copy.
#
# THE FIXTURES USE THE REAL DISPATCHER AND THE REAL INSTALLER. §20's self-heal
# and `axl`'s own verbs are frequently the thing under test, and a fixture that
# stubs the thing under test proves nothing about it. Only `axl-cc` is a stub,
# because compiling is not what any caller of this file is asking about.
#
# Expects from the sourcing test: $PROJECT_DIR, $INSTALLER, $WORK.
#
# Not executable, not a test: sourced only.

make_tree() {  # make_tree <dir> <version>
    local d="$1" v="$2"
    mkdir -p "$d/bin" "$d/share/axl" "$d/libexec/axl"
    # The REAL dispatcher, not a stub: §20's self-heal lives in it, and a
    # fixture that stubs the thing under test proves nothing about it.
    install -m 0755 "$PROJECT_DIR/scripts/axl" "$d/bin/axl"
    echo "placeholder" > "$d/bin/.axl-stub-marker"
    printf '#!/bin/sh\necho "axl-cc %s"\n' "$v" > "$d/bin/axl-cc"
    # THE REAL INSTALLER, under the name a real prefix gives it
    # (install.sh: scripts/install-toolchain.sh -> bin/axl-install-toolchain).
    # A stub here would make `axl toolchain install` prove only that the
    # dispatcher can exec something -- and the branch worth reaching is the
    # already-installed one, where the receipt bug lived.
    install -m 0755 "$PROJECT_DIR/scripts/install-toolchain.sh" \
                    "$d/bin/axl-install-toolchain"
    printf 'not-a-script\n' > "$d/bin/pe-set-debug"
    chmod +x "$d/bin/axl-cc" "$d/bin/pe-set-debug"
    echo "$v" > "$d/share/axl/version"
    make_toolchain_conf "$d/share/axl/axl-toolchains.conf" "$v"
    cp "$INSTALLER" "$d/libexec/axl/install.sh"
    chmod 0644 "$d/libexec/axl/install.sh"
    mark_installer "$d/libexec/axl/install.sh" "$v"
    stage_libexec "$d"
}

# THE TOOLCHAIN MANIFEST, and it pins a DIFFERENT root per SDK version.
#
# The SDK pins the toolchain -- axl-toolchains.conf ships inside the SDK -- so
# "does `axl update` bring the toolchain along" is only observable when two
# versions disagree about which one they pin. A manifest that named one root
# for every version would make the update a no-op and the assertion vacuous.
#
# THE ROOTS SIT DIRECTLY UNDER /opt because install-toolchain.sh's
# relocated_dir() is `basename` under INSTALL_ROOT (default /opt): put them one
# level deeper and the path the manifest spells is not the path the installer
# uses, and every assertion would be about a rewritten path instead of this
# one. They are created by the test, in the container, as ~30-byte shell stubs
# -- the real x64 toolchain is ~239 MB and what is under test is WHICH ARCH the
# update asks for, not the download.
#
# aa64 points at a root nothing ever creates: "x64 installed, aa64 not" is the
# per-arch case the rule exists for, so the fixture has to be able to express
# it.
make_toolchain_conf() {  # make_toolchain_conf <path> <sdk version>
    local f="$1" v="$2"
    cat > "$f" <<CONF
AXL_AA64_TOOLCHAIN_VERSION=fixture-$v
AXL_AA64_TOOLCHAIN_DIR=/opt/axl-fixture-aa64-$v
AXL_AA64_GXX_DEFAULT=/opt/axl-fixture-aa64-$v/bin/aarch64-none-elf-g++
AXL_AA64_GCC_DEFAULT=/opt/axl-fixture-aa64-$v/bin/aarch64-none-elf-gcc
AXL_AA64_BINUTILS_PREFIX_DEFAULT=/opt/axl-fixture-aa64-$v/bin/aarch64-none-elf-
AXL_AA64_TOOLCHAIN_SHA256=0000000000000000000000000000000000000000000000000000000000000000
AXL_X64_TOOLCHAIN_VERSION=fixture-$v
AXL_X64_TOOLCHAIN_DIR=/opt/axl-fixture-x64-$v
AXL_X64_GXX_DEFAULT=/opt/axl-fixture-x64-$v/bin/x86_64-elf-g++
AXL_X64_GCC_DEFAULT=/opt/axl-fixture-x64-$v/bin/x86_64-elf-gcc
AXL_X64_BINUTILS_PREFIX_DEFAULT=/opt/axl-fixture-x64-$v/bin/x86_64-elf-
CONF
}

# The libexec commands, READ BACK from the same make variable
# make-host-tools-tarball.sh uses ("ONE OWNER FOR THE FILE LIST"). Naming them
# here by hand is the drift this tree keeps paying for -- and it bites
# immediately: `axl prune` is a libexec COMMAND discovered by the scan, not a
# verb in the dispatcher, so staging none of them made `axl prune` an unknown
# command and every assertion about pruning pass on the wrong thing. Staging
# only axl-prune.sh was barely better: it sources axl-common.sh, which was not
# there, so every run printed `axl_handle_version: command not found`.
stage_libexec() {  # stage_libexec <prefix-dir>
    local d="$1" f
    mkdir -p "$d/libexec/axl"
    local -a files=()
    mapfile -t files < <(make -s -C "$PROJECT_DIR" print-HOST_TOOL_FILES \
                         | tr ' ' '\n' | grep -v '^$')
    # A gate that checks nothing passes forever: refuse a suspiciously short
    # answer the way release.yml refuses one for the tool set.
    [[ "${#files[@]}" -ge 5 ]] || {
        echo "release-fixture: print-HOST_TOOL_FILES named ${#files[@]} file(s);" \
             "refusing to build a fixture against that" >&2
        return 1
    }
    # They are BARE NAMES under scripts/, which is how make-host-tools-tarball.sh
    # resolves them (`$SDK_DIR/scripts/$_f`). Treating them as paths relative to
    # the repo root skipped all ten silently, and the count guard above did not
    # notice: it counted the LIST, not what actually landed. Count the result.
    local _staged=0
    for f in "${files[@]}"; do
        [[ -f "$PROJECT_DIR/scripts/$f" ]] || continue
        install -m 0755 "$PROJECT_DIR/scripts/$f" "$d/libexec/axl/$f"
        _staged=$((_staged + 1))
    done
    # axl_version.py has no shebang; 0644 keeps it out of the command list,
    # exactly as the real tarball does.
    [[ -f "$d/libexec/axl/axl_version.py" ]] && chmod 0644 "$d/libexec/axl/axl_version.py"
    [[ "$_staged" -eq "${#files[@]}" ]] || {
        echo "release-fixture: staged $_staged of ${#files[@]} libexec file(s)" >&2
        return 1
    }
    return 0
}

# The v4.4.0 host-tools tarball, reproduced: a flat `scripts/` directory with
# no `bin/` and no share/axl/version, unpacked at top level. §14.2 measured
# both defects and §14.1c counted the six top-level entries.
make_legacy_host_tools() {  # make_legacy_host_tools <dir> <version>
    local d="$1" v="$2"
    mkdir -p "$d/scripts"
    printf '#!/bin/bash\necho run-qemu\n' > "$d/scripts/run-qemu.sh"
    chmod +x "$d/scripts/run-qemu.sh"
    printf 'Apache-2.0\n' > "$d/LICENSE"
    printf 'NOTICE\n'     > "$d/NOTICE"
    printf '# changelog\n' > "$d/CHANGELOG.md"
    printf '# host tools\n' > "$d/README.md"
    echo "$v" > "$d/VERSION"
}

# The MANAGER component as it really ships: bin/axl, libexec (with the staged
# installer), share/axl/version. No axl-cc -- that is SDK content.
make_manager_tree() {  # make_manager_tree <dir> <version>
    local d="$1" v="$2"
    mkdir -p "$d/bin" "$d/share/axl" "$d/libexec/axl"
    install -m 0755 "$PROJECT_DIR/scripts/axl" "$d/bin/axl"
    stage_libexec "$d"
    install -m 0644 "$INSTALLER" "$d/libexec/axl/install.sh"
    mark_installer "$d/libexec/axl/install.sh" "$v"
    echo "$v" > "$d/share/axl/version"
}

# WHICH install.sh ACTUALLY RAN. Every version stages a byte-identical copy of
# the one installer in this repo, so "the new installer performed the SDK
# install" -- the entire point of `axl update` re-execing after a self-update
# -- was structurally unobservable: delete the re-exec and every assertion
# still passed. Stamping each staged copy with its own version makes the claim
# checkable. Inserted after the shebang so it fires however the script is
# entered, and on stderr so it cannot be mistaken for installer output a test
# parses.
mark_installer() {  # mark_installer <staged install.sh> <version>
    local f="$1" v="$2"
    sed -i "1a echo \"AXL-INSTALLER-MARKER $v\" >&2" "$f"
    grep -q "AXL-INSTALLER-MARKER $v" "$f" || {
        echo "release-fixture: could not stamp $f" >&2; return 1; }
}

# publish <reldir> <asset> <root|-> <version> [maker]
#   <root> archives the tree under that single top-level directory.
#   '-'    archives its CONTENTS at top level -- a tarbomb, the legacy shape.
publish() {
    local rel="$1" asset="$2" root="$3" v="$4" maker="${5:-make_tree}"
    local stage; stage="$(mktemp -d -p "$WORK")"
    mkdir -p "$rel"
    if [[ "$root" == "-" ]]; then
        "$maker" "$stage" "$v"
        tar -C "$stage" -czf "$rel/$asset" .
    else
        "$maker" "$stage/$root" "$v"
        tar -C "$stage" -czf "$rel/$asset" "$root"
    fi
    rm -rf "$stage"
}

# SHA256SUMS is what install.sh verifies against, and -- after D2 -- what it
# reads to decide WHICH name this release published. Regenerate after every
# publish, exactly as the release job does.
seal() {  # seal <reldir> <version>
    local rel="$1" v="$2"
    echo "$v" > "$rel/VERSION"
    ( cd "$rel" && sha256sum -- *.tar.gz > SHA256SUMS )
}
