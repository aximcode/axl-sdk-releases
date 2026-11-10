# sdk -- the SDK examples, built in this tree as the integration tests boot
# them.
#
# Declared here rather than in sdk/examples/: that directory SHIPS, and its
# own CMakeLists.txt is the consumer's -- a project of its own that finds an
# installed SDK with find_package(axl), which is what a user copies.

set(_p axl_obj_sdk_examples_)

# --- applications and drivers, one source each ------------------------------
foreach(_s
        cancellable-demo cursor-demo echo-client echo-server echo-server-sync
        event-demo frame-anim-demo gfx-demo gfx-window hello input-demo keytrace
        pointer-demo pointer-tune-demo radix-demo reload-svc ring-buf-demo
        runtime-demo tcp-echo-server upgrade-svc)
    axl_obj_name(_n examples/${_s}.c)
    axl_object(${_n} NONE examples/${_s}.c)
    axl_app_image(${_s} ${_n})
endforeach()
foreach(_s binding-driver driver http-server-driver reload-svc-dxe reload-svc-fail-dxe
           smbus-hc-shim)
    axl_obj_name(_n examples/${_s}.c)
    axl_object(${_n} NONE examples/${_s}.c)
    if(_s STREQUAL "smbus-hc-shim")
        axl_driver_image(SmbusHcShim ${_n})
    else()
        axl_driver_image(${_s} ${_n})
    endif()
endforeach()

# embed-asset: a text file, embedded.
axl_object(${_p}embed_asset NONE examples/embed-asset.c)
axl_embed_blob(greeting ${CMAKE_CURRENT_SOURCE_DIR}/examples/embed-asset.txt OBJECT)
axl_app_image(embed-asset ${_p}embed_asset axl_blob_greeting)

# --- services: a launcher embedding its resident driver ----------------------
# <image stem>:<source stem>; the driver is the source with
# AXL_SERVICE_BUILD_DRIVER, the launcher the source as it is.
foreach(_v service_demo:service-demo service_demo_custom:service-demo-custom
           svc_embonly:svc-embonly svc_startfail:svc-startfail)
    string(REPLACE ":" ";" _v ${_v})
    list(GET _v 0 _img)
    list(GET _v 1 _s)
    string(REPLACE "-" "_" _o ${_s})
    axl_object(${_p}${_o} NONE examples/${_s}.c)
    axl_object_variant(${_p}${_o}_dxe NONE examples/${_s}.c AXL_SERVICE_BUILD_DRIVER)
    axl_driver_image(${_img}-dxe ${_p}${_o}_dxe)
    axl_embed_blob(${_img} ${CMAKE_BINARY_DIR}/${_img}-dxe.efi OBJECT
        DEPENDS axl_image_${_img}_dxe)
    axl_app_image(${_img} ${_p}${_o} axl_blob_${_img})
endforeach()
# A second driver svc-embonly's launcher must NOT pick up.
axl_object_variant(${_p}svc_embonly_decoy_dxe NONE examples/svc-embonly.c
    AXL_SERVICE_BUILD_DRIVER DECOY)
axl_driver_image(svc_embonly-decoy-dxe ${_p}svc_embonly_decoy_dxe)

# --- upgrade-svc: the staged-upgrade service and its test variants -----------
# <variant> <definitions>; each is the driver upgrade-svc-dxe-<variant>.
axl_object_variant(${_p}upgrade_svc_dxe NONE examples/upgrade-svc.c AXL_SERVICE_BUILD_DRIVER)
axl_driver_image(upgrade-svc-dxe ${_p}upgrade_svc_dxe)
foreach(_v
        "v2|UPGRADE_SVC_VERSION=\"2\""
        "broken|UPGRADE_SVC_BROKEN;UPGRADE_SVC_VERSION=\"3\""
        "hang|UPGRADE_SVC_HANG;UPGRADE_SVC_VERSION=\"4\""
        "broken2|UPGRADE_SVC_BROKEN;UPGRADE_SVC_VERSION=\"6\""
        "wedge|UPGRADE_SVC_NO_NET;UPGRADE_SVC_VERSION=\"7\";UPGRADE_SVC_WEDGE"
        "q1|UPGRADE_SVC_NO_NET;UPGRADE_SVC_VERSION=\"1\""
        "q2|UPGRADE_SVC_NO_NET;UPGRADE_SVC_VERSION=\"2\""
        "q5|UPGRADE_SVC_NO_NET;UPGRADE_SVC_VERSION=\"5\"")
    string(REPLACE "|" ";" _v "${_v}")
    list(POP_FRONT _v _n)
    axl_object_variant(${_p}upgrade_svc_dxe_${_n} NONE examples/upgrade-svc.c
        AXL_SERVICE_BUILD_DRIVER ${_v})
    axl_driver_image(upgrade-svc-dxe-${_n} ${_p}upgrade_svc_dxe_${_n})
endforeach()
# Up, then -- inside the grace period -- a restage (8), an unstage (9) or a
# hand write over P.new (10): the version is the variant, 8 + IN_GRACE - 1.
foreach(_g 8 9 10)
    math(EXPR _in "${_g} - 7")
    axl_object_variant(${_p}upgrade_svc_dxe_g${_g} NONE examples/upgrade-svc.c
        AXL_SERVICE_BUILD_DRIVER UPGRADE_SVC_NO_NET UPGRADE_SVC_IN_GRACE=${_in}
        UPGRADE_SVC_VERSION="${_g}")
    axl_driver_image(upgrade-svc-dxe-g${_g} ${_p}upgrade_svc_dxe_g${_g})
endforeach()
# The broken image behind the pack stub: its quarantine must refuse it too.
axl_wrap_efi(axl_upgrade_svc_dxe_broken_packed RAW upgrade-svc-dxe-broken.efi
    OUT upgrade-svc-dxe-broken-packed.efi FORCE)
# The test launchers: short timers, and the test hook (a fixture's object).
axl_object_variant(${_p}upgrade_svc_t NONE examples/upgrade-svc.c
    UPGRADE_SVC_WATCHDOG_SEC=30 UPGRADE_SVC_GRACE_SEC=3)
axl_object_variant(${_p}upgrade_svc_t1 NONE examples/upgrade-svc.c
    UPGRADE_SVC_WATCHDOG_SEC=30 UPGRADE_SVC_GRACE_SEC=3 UPGRADE_SVC_TRIES=1)
foreach(_t t t1)
    axl_app_image(upgrade-svc-${_t} ${_p}upgrade_svc_${_t}
        axl_obj_test_integration_upgrade_svc_test_hook)
endforeach()

# --- hello-minimal: NO libaxl, no AXL runtime -- the PE bootstrap and the
# relocator, the program, nothing else (and no pe-set-debug, as the
# Makefile's recipe had none).
axl_object(${_p}hello_minimal_c NONE examples/hello-minimal.c)
# The C++ one compiles with CXXFLAGS alone -- no include path at all.
add_library(${_p}hello_minimal_cxx OBJECT examples/hello-minimal.cpp)
target_link_libraries(${_p}hello_minimal_cxx PRIVATE
    axl_target_flags axl_config_flags axl_cxx_flags)
foreach(_l c cxx)
    axl_efi_image(axl_image_hello_minimal_${_l} EFI hello-minimal-${_l}.efi SUBSYSTEM 10
        NO_LIBS NO_PE_PATCH
        OBJECTS axl_obj_crt0_gcc axl_obj_src_crt0_axl_reloc ${_p}hello_minimal_${_l})
endforeach()
# A global constructor in the C++ one would be REGISTERED AND NEVER RUN:
# there is no runtime to walk .init_array. The Makefile failed its build on
# that; here it is a gate test over the object.
add_test(NAME gate.hello-minimal-no-ctors
    COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${AXL_BINUTILS_PREFIX}objdump
            "-DOBJECTS=$<TARGET_OBJECTS:${_p}hello_minimal_cxx>"
            -P ${CMAKE_SOURCE_DIR}/cmake/AxlCheckNoCtors.cmake)
set_tests_properties(gate.hello-minimal-no-ctors PROPERTIES LABELS "gate")
