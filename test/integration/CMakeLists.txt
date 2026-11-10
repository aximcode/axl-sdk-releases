# test/integration -- the guest-side fixtures the integration tests boot.
#
# Each image sits where the Makefile's prefix put it (<name>.efi at the top,
# drivers/<name>.efi, build/<name>.efi for one embedded in another), so a test
# finds it where it always looked.

# Every *.c here, as the Makefile's wildcard took them, each on its own --
# except the two compiled ONLY with the -D that selects a variant.
file(GLOB _fixtures CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c)
list(REMOVE_ITEM _fixtures pin-svc-driver.c sd-sibling-driver.c)
axl_objects(BUGPRONE ${_fixtures})
axl_source_includes_after(TARGET axl_obj_test_integration_flushfail_fs_driver
    DIRS test/unit SOURCES flushfail-fs-driver.c)
axl_source_includes_after(TARGET axl_obj_test_integration_kbtune_drv_test
    DIRS tools SOURCES kbtune-drv-test.c)

# The variants: <object> <source> <definition>... EVERY one is linted: a
# variant's -D switches code in (svc-tdfail.c's driver half is ~150 lines
# under #ifdef AXL_SERVICE_BUILD_DRIVER), so linting one leaves the rest
# unseen -- and the coverage check counts files, not variants.
set(_p axl_obj_test_integration_)
axl_object_variant(${_p}pin_svc_driver_good BUGPRONE pin-svc-driver.c PIN_VARIANT="good")
axl_object_variant(${_p}pin_svc_driver_shadow BUGPRONE pin-svc-driver.c PIN_VARIANT="shadow")
axl_object_variant(${_p}sd_sibling_driver_a BUGPRONE sd-sibling-driver.c DRIVER_TAG=A)
axl_object_variant(${_p}sd_sibling_driver_b BUGPRONE sd-sibling-driver.c DRIVER_TAG=B)
axl_object_variant(${_p}heap_release_child_leak BUGPRONE heap-release-child.c HEAP_REL_LEAK)
axl_object_variant(${_p}image_path_driver_big BUGPRONE image-path-driver.c
    IMGPATH_PAD_BYTES=262144u)
axl_object_variant(${_p}sshd_test_hook_dxe BUGPRONE sshd-test-hook.c SSHD_TEST_HOOK_DRIVER)
axl_object_variant(${_p}console_device_hold_takeover BUGPRONE console-device-hold.c HOLD_TAKEOVER)
foreach(_v
        "restore_smoke|SELF_UNINSTALL_MS=4000"
        "wide_smoke|AUTO_ALT=true;GRID_COLS=142;GRID_ROWS=44;PRECACHE_SMALL"
        "input_smoke|TAKE_INPUT"
        "input_restore_smoke|SELF_UNINSTALL_MS=4000;TAKE_INPUT"
        "wide_restore_smoke|AUTO_ALT=true;GRID_COLS=142;GRID_ROWS=44;SELF_UNINSTALL_MS=4000"
        "cycle_smoke|AUTO_ALT=true;CYCLE_COUNT=3;GRID_COLS=142;GRID_ROWS=44;SELF_UNINSTALL_MS=1200"
        "passthrough_smoke|PASSTHROUGH_LOCAL")
    string(REPLACE "|" ";" _v "${_v}")
    list(POP_FRONT _v _n)
    axl_object_variant(${_p}console_device_${_n} BUGPRONE console-device-smoke.c ${_v})
endforeach()
# The launcher + resident-driver fixtures: each source's driver half.
foreach(_s stderr-tick svc-tdfail svc-verdict)
    string(REPLACE "-" "_" _n ${_s})
    axl_object_variant(${_p}${_n}_dxe BUGPRONE ${_s}.c AXL_SERVICE_BUILD_DRIVER)
endforeach()
axl_object_variant(${_p}svc_verdict_leak BUGPRONE svc-verdict.c SVC_VERDICT_LEAK)
axl_object_variant(${_p}svc_verdict_leak_dxe BUGPRONE svc-verdict.c
    AXL_SERVICE_BUILD_DRIVER SVC_VERDICT_LEAK)

# C++: the selftests and the constructor-running driver, which its -D picks.
# Not linted, as scripts/lint.sh never linted them.
foreach(_s cxx-json-selftest cxx-seam-selftest cxx-streams-selftest cxx-ctor-driver)
    axl_obj_name(_n ${_s}.cpp)
    axl_object(${_n} NONE ${_s}.cpp)
endforeach()
foreach(_v fe:FAILENTRY fu:FAILUNLOAD sd:SHARED svc:SERVICE)
    string(REPLACE ":" ";" _v ${_v})
    list(GET _v 0 _n)
    list(GET _v 1 _d)
    axl_object_variant(${_p}cxx_ctor_${_n}_driver NONE cxx-ctor-driver.cpp
        AXL_CTOR_FIXTURE_${_d})
endforeach()

# --- applications ------------------------------------------------------------
foreach(_s
        9p-mount-selftest 9p-server-selftest axl-shell-launcher boot-path-selftest
        bss-probe compositor-bench compositor-selftest console-device-present-probe
        console-reshape-selftest console-text-mode-selftest cpu-simd-selftest
        cpu-spin-fixture cpu-topology-selftest cursor-selftest cxx-ctor-test
        driver-identity-test driver-leak-test driver-parent-leak-test
        exit-status-selftest fake-nic-driver-info fs-path-selftest gfx-avail-probe
        gfx-mode-selftest gfx-present-bench gfx-present-selftest gfx-simd-selftest
        heap-release-test http-plain-selftest image-path-test io-streams kbprobe
        kbtune-drv-test key-dump log-redirect-selftest net-gate-probe pack-fixture
        pack-load-test sd-sibling-probe shell-growth-test shell-status-selftest ssh-server-selftest
        timer-res-selftest net-latency-selftest
        stderr-row stderr-stale stdio-bridge-leak stdio-bridge-liveness-test
        stdio-bridge-old stdio-bridge-reap-test stdio-bridge-self
        task-pool-mp-selftest time-settime-selftest vmedia-check)
    axl_obj_name(_n ${_s}.c)
    axl_app_image(${_s} ${_n})
endforeach()
axl_app_image(heap-rel-c ${_p}heap_release_child)
axl_app_image(heap-rel-leak ${_p}heap_release_child_leak)
axl_app_image(heap-rel-sleep ${_p}heap_release_sleep)
axl_app_image(fs-read-probe ${_p}fs_read_probe ${_p}fs_read_common)
axl_app_image(volume-map-test ${_p}volume_map_test axl_blob_ramdiskdxe)
foreach(_s cxx-json-selftest cxx-seam-selftest cxx-streams-selftest)
    axl_obj_name(_n ${_s}.cpp)
    axl_app_image(${_s} CXX ${_n})
endforeach()

# The minimal runtime (LINK_EFI_APP_MINIMAL): the minimal CRT0 instead of
# the AXL one; each `-u` opts the image back into one feature.
foreach(_v
        "exit-status-selftest-minimal|exit_status_selftest|-u;_axl_args_init;-u;axl_stream_init;-u;_axl_log_vdispatch"
        "minimal-log-off|minimal_log_selftest|-u;axl_stream_init"
        "minimal-log-on|minimal_log_selftest|-u;axl_stream_init;-u;_axl_log_vdispatch")
    string(REPLACE "|" ";" _v "${_v}")
    list(POP_FRONT _v _img _obj)
    string(REPLACE "-" "_" _t ${_img})
    axl_efi_image(axl_image_${_t} EFI ${_img}.efi SUBSYSTEM 10
        OBJECTS ${AXL_CRT0_MINIMAL} ${_p}${_obj}
        LINK_FLAGS ${_v})
endforeach()

# --- drivers -----------------------------------------------------------------
foreach(_s
        devhandle-driver flushfail-fs-driver image-path-driver refuse-unload-driver
        reload-svc-stuck-dxe sd-ergo-driver stdio-bridge-driver vmedia-fs-driver)
    axl_obj_name(_n ${_s}.c)
    axl_driver_image(${_s} ${_n})
endforeach()
axl_driver_image(svc_tdfail_foreign ${_p}svc_tdfail_foreign)
axl_driver_image(image-path-driver-big ${_p}image_path_driver_big)
axl_driver_image(fs-read-driver ${_p}fs_read_driver ${_p}fs_read_common)
axl_driver_image(pin-svc-driver-good ${_p}pin_svc_driver_good)
axl_driver_image(pin-svc-driver-shadow ${_p}pin_svc_driver_shadow)
axl_driver_image(sd-sibling-driver-a ${_p}sd_sibling_driver_a)
axl_driver_image(sd-sibling-driver-b ${_p}sd_sibling_driver_b)
axl_driver_image(drivers/console-device-smoke ${_p}console_device_smoke)
axl_driver_image(drivers/console-device-hold ${_p}console_device_hold)
axl_driver_image(drivers/console-device-hold-takeover ${_p}console_device_hold_takeover)
foreach(_n restore-smoke wide-smoke input-smoke input-restore-smoke wide-restore-smoke
           cycle-smoke passthrough-smoke)
    string(REPLACE "-" "_" _o ${_n})
    axl_driver_image(drivers/console-device-${_n} ${_p}console_device_${_o})
endforeach()
foreach(_n ctor fe fu sd svc)
    if(_n STREQUAL "ctor")
        set(_img cxx-ctor-driver)
    else()
        set(_img cxx-ctor-${_n}-driver)
    endif()
    string(REPLACE "-" "_" _o ${_img})
    axl_driver_image(${_img} CXX ${_p}${_o})
endforeach()

# --- launchers, each embedding the driver it starts --------------------------
# <launcher> <its object> <blob name> <the embedded driver> <its objects>...
foreach(_v
        "stderr_tick|stderr_tick|stderr_tick|stderr_tick-dxe|stderr_tick_dxe"
        "svc_tdfail|svc_tdfail|svc_tdfail|svc_tdfail-dxe|svc_tdfail_dxe"
        "svc_verdict|svc_verdict|svc_verdict|svc_verdict-dxe|svc_verdict_dxe"
        "svc_verdict_leak|svc_verdict_leak|svc_verdict_leak|svc_verdict_leak-dxe|svc_verdict_leak_dxe")
    string(REPLACE "|" ";" _v "${_v}")
    list(POP_FRONT _v _img _obj _blob _drv)
    list(TRANSFORM _v PREPEND ${_p})
    axl_driver_image(${_drv} ${_v})
    string(REGEX REPLACE "[/.-]" "_" _dt "${_drv}")
    axl_embed_blob(${_blob} ${CMAKE_BINARY_DIR}/${_drv}.efi OBJECT DEPENDS axl_image_${_dt})
    axl_app_image(${_img} ${_p}${_obj} axl_blob_${_blob})
endforeach()
# Launchers whose driver is an image declared above.
axl_embed_blob(pin_svc_shadow ${CMAKE_BINARY_DIR}/pin-svc-driver-shadow.efi OBJECT
    DEPENDS axl_image_pin_svc_driver_shadow)
axl_app_image(pin-svc-launcher ${_p}pin_svc_launcher axl_blob_pin_svc_shadow)
axl_embed_blob(sd_ergo_driver ${CMAKE_BINARY_DIR}/sd-ergo-driver.efi OBJECT
    DEPENDS axl_image_sd_ergo_driver)
axl_app_image(sd-ergo-launcher ${_p}sd_ergo_launcher axl_blob_sd_ergo_driver)
axl_embed_blob(stdio_bridge_fix ${CMAKE_BINARY_DIR}/stdio-bridge-driver.efi OBJECT
    DEPENDS axl_image_stdio_bridge_driver)
axl_app_image(stdio-bridge-fix ${_p}stdio_bridge_launcher axl_blob_stdio_bridge_fix)

# sshd-t: sshd with a test hook in both halves -- its own server driver,
# embedded in its own launcher.
axl_driver_image(build/sshd-t-dxe axl_obj_tools_sshd_svc_dxe ${_p}sshd_test_hook_dxe
    axl_obj_tools_sshd_bind axl_obj_tools_sshd_sftp_relay)
axl_embed_blob(sshd_dxe ${CMAKE_BINARY_DIR}/build/sshd-t-dxe.efi OBJECT
    TARGET axl_blob_sshd_t_dxe ASM embed-blob-sshd-t-dxe.s DEPENDS axl_image_build_sshd_t_dxe)
axl_app_image(sshd-t axl_obj_tools_sshd axl_obj_tools_sshd_svc axl_obj_tools_sshd_bind
    axl_obj_tools_sshd_conf axl_blob_sshd_t_dxe ${_p}sshd_test_hook)

# --- packed: each force-wrapped (WRAP_EFI_FORCE) -----------------------------
axl_wrap_efi(axl_pack_fixture_w RAW pack-fixture.efi OUT pack-fixture-w.efi FORCE)
axl_wrap_efi(axl_pack_driver_w RAW image-path-driver.efi OUT pack-driver-w.efi FORCE)
axl_wrap_efi(axl_pack_sdsib_drv_w RAW sd-sibling-driver-a.efi OUT pack-sdsib-drv-w.efi
    FORCE)
axl_wrap_efi(axl_pack_kbtune_drv_w RAW tools/raw/kbtune-drv.efi OUT pack-kbtune-drv-w.efi
    FORCE)
axl_wrap_efi(axl_pack_crashtest_w RAW tools/raw/crashtest.efi OUT pack-crashtest-w.efi
    FORCE)
# A file another directory's custom command makes is not a dependency CMake
# orders by: the target that makes it is.
add_dependencies(axl_pack_kbtune_drv_w axl_image_kbtune_drv)
add_dependencies(axl_pack_crashtest_w axl_image_crashtest)
# ...and behind the TEST stub, whose first decode sees a damaged payload.
axl_wrap_efi(axl_pack_fixture_rt RAW pack-fixture.efi OUT pack-fixture-rt.efi FORCE
    STUB ${AXL_PACK_STUB_TEST_OBJS})

# ---------------------------------------------------------------------------
# The integration tests as CTest tests (slice 5a, design §10.8): registered at
# ctest time by cmake/AxlIntegrationTests.cmake.in, in the DEBUG trees only --
# the suite runs against the debug build, as run-integration.sh's does. Not
# in the lint tree (its tests are the lint gates) nor in an RTK tree (a
# variant build of two images, not a tree the suite runs against).
# ---------------------------------------------------------------------------
if(AXL_INTEGRATION_TESTS)   # decided in the root CMakeLists.txt
    set(AXL_IT_SOURCE_DIR ${CMAKE_SOURCE_DIR})
    set(AXL_IT_BINARY_DIR ${CMAKE_BINARY_DIR})
    if(AXL_ARCH STREQUAL "aa64")
        set(AXL_IT_ARCH AARCH64)
    else()
        set(AXL_IT_ARCH X64)
    endif()
    # For the artifact gates (cmake/AxlIntegrationTests.cmake.in): the crt0
    # check-bss-clear disassembles, and the runtime archives every link
    # carries, which check-no-avx scans beside the tree's own images.
    set(AXL_BSS_CRT0 ${AXL_HOST_DIR}/axl-crt0-gcc-${CMAKE_SYSTEM_PROCESSOR}.o)
    set(AXL_RUNTIME_ARCHIVES "")
    foreach(_a ${AXL_LIBSTDCXX_A} ${AXL_LIBSUPCXX_A} ${AXL_LIBC_A} ${AXL_LIBM_A} ${AXL_LIBGCC_A})
        string(APPEND AXL_RUNTIME_ARCHIVES " \"${_a}\"")
    endforeach()
    configure_file(${CMAKE_SOURCE_DIR}/cmake/AxlIntegrationTests.cmake.in
        ${CMAKE_BINARY_DIR}/axl-integration-ctest.cmake @ONLY)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        ${CMAKE_BINARY_DIR}/axl-integration-ctest.cmake)
endif()
