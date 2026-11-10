# AxlPackage.cmake -- the release archives: what each holds (install()
# components) and how CPack packs it (one config per archive).
# docs/AXL-Build-System-Design.md §10.16.
#
# THREE ARCHIVES, the asset contract (AXL-Distribution-Design.md §14.1a; the
# names and top-level layout pinned by test-release-asset-names.sh):
#
#   axl-sdk-linux-<v>-<host>.tar.gz    root axl-sdk-<v>/: the SDK prefix
#       (cmake/AxlInstall.cmake's sdk_* components, from BOTH arches' trees)
#       plus sdk_doc: the licences and documentation the retired .deb/.rpm
#       carried, sdk/examples as source, and a root VERSION
#   axl-sdk-uefi-tools-<v>-<arch>.tar.gz  root axl-sdk-uefi-tools-<v>-<arch>/:
#       the shipped .efi tools, sidecars, drivers/<arch>/, licences, README
#       -- component uefi_tools of packaging/uefi-tools/CMakeLists.txt, which
#       reads one arch's built tree
#   axl-sdk-host-tools-<v>.tar.gz      root axl-sdk-host-tools-<v>/: bin/axl,
#       libexec/axl/ and the installer -- component host_tools of
#       packaging/host-tools/CMakeLists.txt, a project with no compiler, so
#       that archive needs no toolchain
#
# REPRODUCIBLE: `cpack -G External` stages the components and
# cmake/AxlPackTar.cmake packs the staged root with a sorted, numeric-owner
# tar at the commit's SOURCE_DATE_EPOCH, so two builders on two filesystems
# produce the same bytes (test-release-reproducible.sh). CPack's own TGZ
# writer is not used: it records the builder's user and group NAMES and
# lists entries in readdir order (§10.9, §10.18).
#
# Installs that depend on the archive being made, not on the tree, read the
# environment make-release-assets.sh sets: AXL_RELEASE_VERSION (the
# uefi-tools version, which a test pre-release overrides) and AXL_RTK_OUT
# (where sshd-rtk.efi was built, only with --with-rtk).

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# sdk_doc -- THE DOCUMENTATION AND LICENCE PAYLOAD of the SDK archive. The
# archive that ships libaxl.a owes the obligations of everything compiled into
# it: mbedtls (Apache-2.0 §4(a)), dejavu (Bitstream Vera), libvterm (MIT),
# freetype (FTL's credit clause), edk2 (BSD-2-Clause-Patent, RamDiskDxe in
# mkrd.efi); ed25519 is public domain, its provenance staged anyway. NOTICE is
# Apache-2.0 §4(d), and states the FreeType and libvterm notices.
# ---------------------------------------------------------------------------
set(_axl_doc share/doc/axl-sdk)
install(FILES
        ${CMAKE_SOURCE_DIR}/CHANGELOG.md ${CMAKE_SOURCE_DIR}/README.md
        ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/NOTICE
        ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md
    DESTINATION ${_axl_doc} COMPONENT sdk_doc)
# The examples as SOURCE: build output in a dirty checkout has no business in
# a release. `*.txt` keeps embed-asset.txt, embed-asset.c's --embed input.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sdk/examples DESTINATION ${_axl_doc} COMPONENT sdk_doc
    FILES_MATCHING PATTERN "*.c" PATTERN "*.h" PATTERN "*.cpp" PATTERN "*.hpp"
    PATTERN "*.md" PATTERN "*.txt")
foreach(_pair
        "mbedtls|deps/mbedtls/LICENSE"
        "dejavu|third_party/dejavu/LICENSE"
        "libvterm|deps/libvterm/LICENSE"
        "freetype|deps/freetype/FTL.TXT;deps/freetype/LICENSE.TXT"
        "edk2|third_party/edk2/LICENSE;third_party/edk2/README.md"
        "ed25519|deps/ed25519-ref10/README.md")
    string(REPLACE "|" ";" _pair "${_pair}")
    list(POP_FRONT _pair _dest)
    list(TRANSFORM _pair PREPEND ${CMAKE_SOURCE_DIR}/)
    install(FILES ${_pair} DESTINATION ${_axl_doc}/third_party/${_dest} COMPONENT sdk_doc)
endforeach()
# A VERSION at the root, for a human holding an extracted tree
# (share/axl/version is what the tools read).
install(FILES ${CMAKE_BINARY_DIR}/install/version DESTINATION . RENAME VERSION
    COMPONENT sdk_doc)

# ---------------------------------------------------------------------------
# The CPack config every archive shares (`cpack --config <file>`). Which
# archive -- name, versioned root, and the trees and components it installs
# (the SDK's span both arches' trees, which one tree cannot name) -- is
# passed by make-release-assets.sh.
# ---------------------------------------------------------------------------
set(AXL_CMAKE_DIR "${CMAKE_SOURCE_DIR}/cmake")
configure_file(${CMAKE_SOURCE_DIR}/cmake/CPackConfig.cmake.in
    ${CMAKE_BINARY_DIR}/CPackConfig-release.cmake @ONLY)

# ---------------------------------------------------------------------------
# The .deb and .rpm (AXL-Distribution-Design.md §17.5): the SDK archive's
# tree, installed at /opt/axl-sdk-<v> by a package named axl-sdk-<v>, plus
# what only a package carries -- its receipt (share/axl/.axl-receipt, SOURCE
# deb or rpm: `axl prune` and install.sh keep their hands off the root, and
# the manager verbs refuse there) and the profile.d script its alternatives
# slave links. One component per format, because the receipt differs.
# ---------------------------------------------------------------------------
foreach(_axl_fmt deb rpm)
    file(WRITE ${CMAKE_BINARY_DIR}/install/pkg-${_axl_fmt}/.axl-receipt
        "AXL_RECEIPT_KIND=sdk\nAXL_RECEIPT_SOURCE=${_axl_fmt}\n")
    install(FILES ${CMAKE_BINARY_DIR}/install/pkg-${_axl_fmt}/.axl-receipt
        DESTINATION share/axl COMPONENT pkg_${_axl_fmt})
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/system/profile.sh
        DESTINATION share/axl COMPONENT pkg_${_axl_fmt})
endforeach()
# The maintainer scripts. The alternatives priority is the version, so the
# newest installed package is current: MAJOR*10000 + MINOR*100 + PATCH.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _axl_m "${_axl_ver}")
math(EXPR AXL_ALT_PRIORITY "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")
set(AXL_VERSION "${_axl_ver}")
foreach(_axl_s postinst prerm)
    configure_file(${CMAKE_SOURCE_DIR}/packaging/system/${_axl_s}.in
        ${CMAKE_BINARY_DIR}/install/pkg/${_axl_s} @ONLY
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)
endforeach()
include(${CMAKE_SOURCE_DIR}/cmake/AxlQemuDeps.cmake)
string(REPLACE " " ", " AXL_DEB_RECOMMENDS "${AXL_HOST_DEPS_DEB}")
string(REPLACE " " ", " AXL_RPM_RECOMMENDS "${AXL_HOST_DEPS_DNF}")
set(AXL_PKG_DIR "${CMAKE_BINARY_DIR}/install/pkg")
configure_file(${CMAKE_SOURCE_DIR}/packaging/system/CPackConfig-package.cmake.in
    ${CMAKE_BINARY_DIR}/CPackConfig-package.cmake @ONLY)
