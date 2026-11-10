# test/unit -- the unit test binaries (AxlTest*.efi), which test-axl.sh boots.

# Each source on its own: each binary links its own object. The extra include
# directories come from ONE interface library linked after axl_flags (searched
# after the common ones, as the Makefile's TEST_CFLAGS appended them).
add_library(axl_unit_test_includes INTERFACE)
target_include_directories(axl_unit_test_includes INTERFACE
    ${CMAKE_SOURCE_DIR}/test/unit ${CMAKE_SOURCE_DIR}/test/data
    ${CMAKE_SOURCE_DIR}/src/ata ${CMAKE_SOURCE_DIR}/src/hii)
file(GLOB _tests CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c)
foreach(_src IN LISTS _tests)
    axl_obj_name(_n ${_src})
    axl_object(${_n} BUGPRONE ${_src})
    target_link_libraries(${_n} PRIVATE axl_unit_test_includes)
endforeach()

# The binaries, as the Makefile's BUILD_TEST linked them (LINK_EFI_TEST): the
# test CRT0 -- entry stub, relocator, debug info, but NOT the AXL application
# CRT0 -- the test, then anything it shares with a tool. Name, source stem:
set(AXL_UNIT_TESTS
    AxlTestMem axl-test-mem
    AxlTestString axl-test-string
    AxlTestIO axl-test-io
    AxlTestLog axl-test-log
    AxlTestData axl-test-data
    AxlTestUtil axl-test-util
    AxlTestLoop axl-test-loop
    AxlTestTask axl-test-task
    AxlTestNet axl-test-net
    AxlTestSmbus axl-test-smbus
    AxlTestIpmi axl-test-ipmi
    AxlTestPlatform axl-test-platform
    AxlTestEvent axl-test-event
    AxlTestCpuIdle axl-test-cpu-idle
    AxlTestRuntime axl-test-runtime
    AxlTestXml axl-test-xml
    AxlTestFsProvider axl-test-fs-provider
    AxlTestGfx axl-test-gfx
    AxlTestTruetype axl-test-truetype
    AxlTestPixmap axl-test-pixmap
    AxlTestMath axl-test-math
    AxlTestInput axl-test-input
    AxlTestFileView axl-test-file-view
    AxlTestPieceTree axl-test-piece-tree
    AxlTestFind axl-test-find
    AxlTestVterm axl-test-vterm
    AxlTestDriver axl-test-driver
    AxlTestCursor axl-test-cursor
    AxlTestCompositor axl-test-compositor
    AxlTestGfxRegion axl-test-gfx-region
    AxlTestCrypto axl-test-crypto
    AxlTestJose axl-test-jose
    AxlTestNvme axl-test-nvme
    AxlTestAta axl-test-ata
    AxlTestScsi axl-test-scsi
    AxlTestSmart axl-test-smart
    AxlTestHii axl-test-hii
    AxlTestAuth axl-test-auth
    AxlTestFw axl-test-fw
    AxlTestPack axl-test-pack
    AxlTestReplace axl-test-replace
    AxlTest9p axl-test-9p
    AxlTestJsonConformance axl-test-json-conformance
    AxlTestSsh axl-test-ssh
    AxlTestEd25519 axl-test-ed25519
    AxlTestJsonCorpus axl-test-json-corpus
    AxlTestSshHostKey axl-test-ssh-host-key
    AxlTestSshHostKeyWeak axl-test-ssh-host-key-weak
    AxlTestSshdBind axl-test-sshd-bind
    AxlTestSshdConf axl-test-sshd-conf
    AxlTestSshdSftp axl-test-sshd-sftp
)
set(AXL_TEST_EXTRA_AxlTestSshdBind axl_obj_tools_sshd_bind)
set(AXL_TEST_EXTRA_AxlTestSshdConf axl_obj_tools_sshd_conf axl_obj_tools_sshd_bind axl_obj_tools_sshd_svc)
set(AXL_TEST_EXTRA_AxlTestSshdSftp axl_obj_tools_sshd_sftp_codec axl_obj_tools_sshd_sftp)
list(LENGTH AXL_UNIT_TESTS _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET AXL_UNIT_TESTS ${_i} _name)
    list(GET AXL_UNIT_TESTS ${_j} _stem)
    axl_obj_name(_obj ${_stem}.c)
    axl_efi_image(axl_image_${_name} EFI ${_name}.efi SUBSYSTEM 10
        OBJECTS ${AXL_CRT0_DRIVER} ${_obj} ${AXL_TEST_EXTRA_${_name}}
        LINK_FLAGS -u _axl_log_vdispatch)
endforeach()
