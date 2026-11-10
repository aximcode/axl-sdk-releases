# tools -- the shipped tools.

# Each tool source on its own, as its image links it.
axl_objects(BUGPRONE
    9p-cmd-file.c
    9p-cmd-mount.c
    9p-cmd-serve.c
    9p-common.c
    9p-mount-svc.c
    9p-serve-svc.c
    9p.c
    agent.c
    ata.c
    axbench.c
    cat.c
    clip.c
    crashtest.c
    cut.c
    dmidecode.c
    fbcon-drv.c
    fbcon.c
    fetch.c
    find.c
    fwtool.c
    grep.c
    hexdump.c
    i2c.c
    ipmi.c
    kbtune-drv.c
    kbtune.c
    lsacpi.c
    lspci.c
    lsproto.c
    lsusb.c
    md5sum.c
    memspd.c
    mkfixture.c
    mkrd.c
    netinfo.c
    netload.c
    nvme.c
    paste.c
    rfbrowse.c
    rndisfix.c
    scsi.c
    sed.c
    sftp-server.c
    sha256sum.c
    smart.c
    sshd-bind.c
    sshd-conf.c
    sshd-sftp-codec.c
    sshd-sftp-relay.c
    sshd-sftp.c
    sshd-svc.c
    sshd.c
    sum.c
    sysinfo.c
    tar.c
    timetest.c
    tr.c)

# The shipped tools, in TOOL_NAMES order (scripts/axl-lists.conf) -- the order
# axl.efi links its applets in, so it is part of that image.
axl_list(AXL_TOOL_NAMES TOOL_NAMES)
set(AXL_TOOL_EXTRA_md5sum axl_obj_tools_sum)
set(AXL_TOOL_EXTRA_sha256sum axl_obj_tools_sum)
list(TRANSFORM AXL_TOOL_NAMES APPEND .c OUTPUT_VARIABLE AXL_BUSYBOX_SOURCES)

# axl.efi, the multi-call image: its dispatcher and every applet, compiled
# with -DAXL_BUSYBOX. The dispatcher is a target of its own because it is
# compiled ONLY this way, so it is the copy lint must see; the applets are
# linted through their plain builds above.
axl_object(axl_busybox_main_objs BUGPRONE axl.c)
axl_object(axl_busybox_objs NONE ${AXL_BUSYBOX_SOURCES})
foreach(_t axl_busybox_main_objs axl_busybox_objs)
    target_compile_definitions(${_t} PRIVATE AXL_BUSYBOX)
endforeach()

# The driver half of each launcher + resident-driver pair, one source each
# (axl_obj_tools_<stem>_dxe): each links on its own into the driver image
# its launcher embeds. The launcher half is the plain build above.
foreach(_src 9p-mount-svc.c 9p-serve-svc.c sshd-svc.c)
    axl_obj_name(_n ${_src})
    axl_object(${_n}_dxe BUGPRONE ${_src})
    target_compile_definitions(${_n}_dxe PRIVATE AXL_SERVICE_BUILD_DRIVER)
endforeach()

# sshd's -full build carries the SFTP subsystem in-process.
axl_object(axl_sshd_full_objs NONE sshd-sftp-relay.c)
target_compile_definitions(axl_sshd_full_objs PRIVATE SSHD_EMBEDDED_SFTP)

# --- images ------------------------------------------------------------------
# mkrd, axl.efi (and a fixture) carry the vendored EDK2 RamDiskDxe, embedded.
axl_embed_blob(ramdiskdxe ${CMAKE_SOURCE_DIR}/third_party/edk2/RamDiskDxe-${AXL_ARCH}.efi OBJECT)

# The shipped tools, linked as the Makefile's BUILD_TOOL linked them: the
# application CRT0, the tool, and what it shares (AXL_TOOL_EXTRA_<n>). The raw
# image is tools/raw/<n>.efi with its symbols at tools/<n>.so; the shipped
# tools/<n>.efi is the packed form (axl_wrap_efi, below).
set(AXL_TOOL_EXTRA_mkrd axl_blob_ramdiskdxe)
foreach(_t IN LISTS AXL_TOOL_NAMES)
    axl_efi_image(axl_image_${_t} EFI tools/raw/${_t}.efi SO tools/${_t}.so
        SUBSYSTEM 10 OBJECTS ${AXL_CRT0_APP} axl_obj_tools_${_t} ${AXL_TOOL_EXTRA_${_t}}
        LINK_FLAGS -u _axl_log_vdispatch)
endforeach()

# --- the shipped extras: their own link lines, as the Makefile's rules -----
set(AXL_APP_LINK -u _axl_log_vdispatch)
set(AXL_DRIVER_LINK -u _axl_log_vdispatch --defsym=_AxlEntry=DriverEntry)

axl_efi_image(axl_image_crashtest EFI tools/raw/crashtest.efi SO tools/crashtest.so
    SUBSYSTEM 10 OBJECTS ${AXL_CRT0_APP} axl_obj_tools_crashtest
    LINK_FLAGS ${AXL_APP_LINK})
axl_efi_image(axl_image_agent EFI tools/raw/agent.efi SO tools/agent.so
    SUBSYSTEM 10 OBJECTS ${AXL_CRT0_APP} axl_obj_tools_agent
    LINK_FLAGS ${AXL_APP_LINK})

# kbtune-drv and SftpServer are drivers in their own right.
axl_efi_image(axl_image_kbtune_drv EFI tools/raw/kbtune-drv.efi SO tools/kbtune-drv.so
    SUBSYSTEM 11 OBJECTS ${AXL_CRT0_DRIVER} axl_obj_tools_kbtune_drv
    LINK_FLAGS ${AXL_DRIVER_LINK})
axl_efi_image(axl_image_SftpServer EFI tools/raw/SftpServer.efi SO tools/SftpServer.so
    SUBSYSTEM 11 OBJECTS ${AXL_CRT0_DRIVER} axl_obj_tools_sftp_server
                         axl_obj_tools_sshd_sftp axl_obj_tools_sshd_sftp_codec
    LINK_FLAGS ${AXL_DRIVER_LINK})

# fbcon: the launcher, embedding its console driver.
axl_efi_image(axl_image_fbcon_drv EFI build/fbcon-drv.efi SUBSYSTEM 11
    OBJECTS ${AXL_CRT0_DRIVER} axl_obj_tools_fbcon_drv
    LINK_FLAGS ${AXL_DRIVER_LINK})
axl_embed_blob(fbcon_drv ${CMAKE_BINARY_DIR}/build/fbcon-drv.efi OBJECT
    DEPENDS axl_image_fbcon_drv)
axl_efi_image(axl_image_fbcon EFI tools/raw/fbcon.efi SO tools/fbcon.so SUBSYSTEM 10
    OBJECTS ${AXL_CRT0_APP} axl_obj_tools_fbcon axl_blob_fbcon_drv
    LINK_FLAGS ${AXL_APP_LINK})

# 9p: one launcher carrying the server and the mount, each a resident driver.
foreach(_d serve mount)
    axl_efi_image(axl_image_9p_${_d}_dxe EFI build/9p-${_d}-dxe.efi SUBSYSTEM 11
        OBJECTS ${AXL_CRT0_DRIVER} axl_obj_tools_9p_${_d}_svc_dxe
        LINK_FLAGS ${AXL_DRIVER_LINK})
    axl_embed_blob(${_d}9p_dxe ${CMAKE_BINARY_DIR}/build/9p-${_d}-dxe.efi OBJECT
        DEPENDS axl_image_9p_${_d}_dxe)
endforeach()
axl_efi_image(axl_image_9p EFI tools/raw/9p.efi SO tools/9p.so SUBSYSTEM 10
    OBJECTS ${AXL_CRT0_APP} axl_obj_tools_9p axl_obj_tools_9p_common
            axl_obj_tools_9p_cmd_file axl_obj_tools_9p_cmd_serve axl_obj_tools_9p_serve_svc
            axl_obj_tools_9p_cmd_mount axl_obj_tools_9p_mount_svc
            axl_blob_serve9p_dxe axl_blob_mount9p_dxe
    LINK_FLAGS ${AXL_APP_LINK})

# sshd: the launcher, embedding the server driver; the server relays SFTP to
# SftpServer.efi beside it.
axl_efi_image(axl_image_sshd_dxe EFI build/sshd-dxe.efi SUBSYSTEM 11
    OBJECTS ${AXL_CRT0_DRIVER} axl_obj_tools_sshd_svc_dxe axl_obj_tools_sshd_bind
            axl_obj_tools_sshd_sftp_relay
    LINK_FLAGS ${AXL_DRIVER_LINK})
axl_embed_blob(sshd_dxe ${CMAKE_BINARY_DIR}/build/sshd-dxe.efi OBJECT
    DEPENDS axl_image_sshd_dxe)
set(AXL_SSHD_APP ${AXL_CRT0_APP} axl_obj_tools_sshd axl_obj_tools_sshd_svc
    axl_obj_tools_sshd_bind axl_obj_tools_sshd_conf)
axl_efi_image(axl_image_sshd EFI tools/raw/sshd.efi SO tools/sshd.so SUBSYSTEM 10
    OBJECTS ${AXL_SSHD_APP} axl_blob_sshd_dxe
    LINK_FLAGS ${AXL_APP_LINK})

# sshd-full: the same launcher, its driver carrying SftpServer in-process
# (the raw image, embedded) instead of relaying to the file beside it.
axl_embed_blob(sftp_server_raw ${CMAKE_BINARY_DIR}/tools/raw/SftpServer.efi OBJECT
    DEPENDS axl_image_SftpServer)
axl_efi_image(axl_image_sshd_full_dxe EFI build/sshd-full/sshd-dxe.efi SUBSYSTEM 11
    OBJECTS ${AXL_CRT0_DRIVER} axl_obj_tools_sshd_svc_dxe axl_obj_tools_sshd_bind
            axl_sshd_full_objs axl_blob_sftp_server_raw
    LINK_FLAGS ${AXL_DRIVER_LINK})
axl_embed_blob(sshd_dxe ${CMAKE_BINARY_DIR}/build/sshd-full/sshd-dxe.efi OBJECT
    TARGET axl_blob_sshd_full_dxe ASM embed-sshd-dxe.s DEPENDS axl_image_sshd_full_dxe)
axl_efi_image(axl_image_sshd_full EFI tools/raw/sshd-full.efi SO tools/sshd-full.so
    SUBSYSTEM 10 OBJECTS ${AXL_SSHD_APP} axl_blob_sshd_full_dxe
    LINK_FLAGS ${AXL_APP_LINK})

# sshd-rtk / sshd-full-rtk: sshd with a NIC driver inside it -- Realtek's
# RtkUsbUndiDxe.efi, a binary that never enters this tree. Built ONLY when
# AXL_RTK_USB_UNDI names the driver (any driver file is accepted: the test
# embeds an e1000). It is copied in, packed (the pack wrap), and spliced with
# its file name into a second build of the server driver
# (-DSSHD_EMBEDDED_NIC_DRIVER); the launchers are sshd's own objects around it.
set(AXL_RTK_USB_UNDI "" CACHE FILEPATH "NIC driver to embed in sshd-rtk (axl-build.sh's RTK_USB_UNDI)")
if(NOT AXL_RTK_USB_UNDI)
    message(STATUS "sshd-rtk: not built -- it embeds a NIC driver this tree does not "
                   "carry: -DAXL_RTK_USB_UNDI=/path/<arch>/RtkUsbUndiDxe.efi")
    # The goals still exist, and say so, as the Makefile's did: the integration
    # runner builds every test's goals before it starts, this one included.
    foreach(_t sshd-rtk sshd-full-rtk)
        add_custom_target(${_t} COMMAND ${CMAKE_COMMAND} -E echo
            "  ${_t}: not built -- it embeds a NIC driver this tree does not carry:"
            "scripts/axl-build.sh ${_t} RTK_USB_UNDI=/path/<arch>/RtkUsbUndiDxe.efi"
            VERBATIM)
    endforeach()
else()
    set(_rtk ${CMAKE_BINARY_DIR}/build/sshd-rtk)
    add_custom_command(OUTPUT ${_rtk}/nic.src.efi
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AXL_RTK_USB_UNDI} ${_rtk}/nic.src.efi
        DEPENDS ${AXL_RTK_USB_UNDI} VERBATIM)
    add_custom_target(axl_rtk_nic_src DEPENDS ${_rtk}/nic.src.efi)
    axl_image_target(build/sshd-rtk/nic.src.efi axl_rtk_nic_src)
    get_filename_component(_nic_name ${AXL_RTK_USB_UNDI} NAME)
    file(CONFIGURE OUTPUT ${_rtk}/nic.name CONTENT "${_nic_name}")
    axl_wrap_efi(axl_rtk_nic RAW build/sshd-rtk/nic.src.efi
        OUT build/sshd-rtk/nic.efi)
    add_dependencies(axl_rtk_nic axl_rtk_nic_src)
    axl_embed_blob(sshd_nic_driver ${_rtk}/nic.efi OBJECT DEPENDS axl_rtk_nic)
    axl_embed_blob(sshd_nic_name ${_rtk}/nic.name OBJECT)

    axl_object(axl_obj_tools_sshd_svc_dxe_rtk NONE sshd-svc.c)
    target_compile_definitions(axl_obj_tools_sshd_svc_dxe_rtk PRIVATE
        AXL_SERVICE_BUILD_DRIVER SSHD_EMBEDDED_NIC_DRIVER)
    set(_rtk_drv ${AXL_CRT0_DRIVER} axl_obj_tools_sshd_svc_dxe_rtk axl_obj_tools_sshd_bind)
    set(_nic axl_blob_sshd_nic_driver axl_blob_sshd_nic_name)

    axl_efi_image(axl_image_sshd_rtk_dxe EFI build/sshd-rtk/sshd-dxe.efi SUBSYSTEM 11
        OBJECTS ${_rtk_drv} axl_obj_tools_sshd_sftp_relay ${_nic}
        LINK_FLAGS ${AXL_DRIVER_LINK})
    axl_embed_blob(sshd_dxe ${_rtk}/sshd-dxe.efi OBJECT TARGET axl_blob_sshd_rtk_dxe
        ASM sshd-rtk/embed-sshd-dxe.s DEPENDS axl_image_sshd_rtk_dxe)
    axl_efi_image(axl_image_sshd_rtk EFI tools/raw/sshd-rtk.efi SO tools/sshd-rtk.so
        SUBSYSTEM 10 OBJECTS ${AXL_SSHD_APP} axl_blob_sshd_rtk_dxe
        LINK_FLAGS ${AXL_APP_LINK})

    axl_efi_image(axl_image_sshd_full_rtk_dxe EFI build/sshd-rtk/full/sshd-dxe.efi
        SUBSYSTEM 11
        OBJECTS ${_rtk_drv} axl_sshd_full_objs ${_nic} axl_blob_sftp_server_raw
        LINK_FLAGS ${AXL_DRIVER_LINK})
    axl_embed_blob(sshd_dxe ${_rtk}/full/sshd-dxe.efi OBJECT TARGET axl_blob_sshd_full_rtk_dxe
        ASM sshd-rtk/full/embed-sshd-dxe.s DEPENDS axl_image_sshd_full_rtk_dxe)
    axl_efi_image(axl_image_sshd_full_rtk EFI tools/raw/sshd-full-rtk.efi
        SO tools/sshd-full-rtk.so SUBSYSTEM 10
        OBJECTS ${AXL_SSHD_APP} axl_blob_sshd_full_rtk_dxe
        LINK_FLAGS ${AXL_APP_LINK})
    foreach(_t sshd-rtk sshd-full-rtk)
        axl_wrap_efi(axl_ship_${_t} RAW tools/raw/${_t}.efi OUT tools/${_t}.efi)
    endforeach()
    # The goals: the shipped image, copied to $SSHD_RTK_OUT when the caller
    # names one, reported in the Makefile's words (cmake/AxlRtkOut.cmake).
    set(_label_sshd-rtk "sshd + embedded ${_nic_name}")
    set(_label_sshd-full-rtk "sshd + SftpServer.efi + ${_nic_name}")
    foreach(_t sshd-rtk sshd-full-rtk)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -DIMG=${CMAKE_BINARY_DIR}/tools/${_t}.efi
                    "-DLABEL=${_label_${_t}}" -P ${CMAKE_SOURCE_DIR}/cmake/AxlRtkOut.cmake
            VERBATIM)
        add_dependencies(${_t} axl_ship_${_t})
    endforeach()
endif()

# The SHIPPED tools/<n>.efi: each raw image through the pack wrap -- packed
# when that pays, else a byte copy -- in DEBUG and RELEASE alike, so a test
# that boots a tool boots what ships.
# SHIPPED_TOOL_NAMES (scripts/axl-lists.conf): TOOL_NAMES, TOOL_EXTRA_APPS
# and three.
axl_list(AXL_SHIPPED_TOOLS SHIPPED_TOOL_NAMES)
foreach(_t IN LISTS AXL_SHIPPED_TOOLS)
    axl_wrap_efi(axl_ship_${_t} RAW tools/raw/${_t}.efi OUT tools/${_t}.efi)
endforeach()

# `tool-sizes`: the shipped set built, then a size report of it
# (cmake/AxlToolSizes.cmake). A goal of its own, not an image list: what the
# caller reads is the report.
add_custom_target(tool-sizes
    COMMAND ${CMAKE_COMMAND} -DPREFIX=${CMAKE_BINARY_DIR} -DARCH=${AXL_ARCH}
            "-DTOOLS=$<JOIN:${AXL_SHIPPED_TOOLS},;>"
            -P ${CMAKE_SOURCE_DIR}/cmake/AxlToolSizes.cmake
    VERBATIM)
add_dependencies(tool-sizes tools)

# axl.efi, the multi-call image: every applet in TOOL_NAMES order, the
# dispatcher last, then what the applets share -- sum.o and the RamDiskDxe blob.
axl_efi_image(axl_image_busybox EFI axl.efi SUBSYSTEM 10
    OBJECTS ${AXL_CRT0_APP} axl_busybox_objs axl_busybox_main_objs
            axl_obj_tools_sum axl_blob_ramdiskdxe
    LINK_FLAGS -u _axl_log_vdispatch)

# The release's drivers/<arch>/ set (goal release-drivers): every vendored
# third_party/edk2/<Name>-<arch>.efi as <Name>.efi, through the pack wrap.
# They save less than the stub costs, so each ships as a byte copy.
file(GLOB _edk2 CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/third_party/edk2
     ${CMAKE_SOURCE_DIR}/third_party/edk2/*-${AXL_ARCH}.efi)
foreach(_f IN LISTS _edk2)
    string(REGEX REPLACE "-${AXL_ARCH}\\.efi$" "" _n ${_f})
    axl_wrap_efi(axl_release_driver_${_n} RAW ${CMAKE_SOURCE_DIR}/third_party/edk2/${_f}
        OUT release-drivers/${AXL_ARCH}/${_n}.efi)
endforeach()

# fwtool-host: fwtool for the BUILD machine (HOSTCC), over the same
# backend-free parser and LZMA sources as fwtool.efi, so a golden test
# (test-fwtool-host.sh) can hold the C parser to scripts/extract-fv-shell.py
# on real firmware. The shim header maps AXL's leaf primitives onto libc;
# -DAXL_HOSTED selects fwtool.c's libc I/O; --gc-sections drops the paths
# (gzip, the LZMA encoder) the decode-only extract never reaches.
axl_host_tool(fwtool-host
    SOURCES tools/fwtool.c src/fw/axl-fw.c src/data/axl-compress.c
            src/data/axl-compress-lzma.c deps/lzma/LzmaDec.c deps/lzma/LzmaEnc.c
            deps/lzma/LzFind.c
    FLAGS -Wall -O2 -DAXL_HOSTED -DZ7_ST -ffunction-sections -fdata-sections
          -Wl,--gc-sections -include ${CMAKE_SOURCE_DIR}/tools/fwtool-host-shim.h
          -I${CMAKE_SOURCE_DIR}/include -I${CMAKE_SOURCE_DIR}/deps/lzma
    DEPENDS tools/fwtool-host-shim.h)
set(_fwtool_host ${AXL_HOST_TOOL_fwtool-host})
add_custom_target(axl_fwtool_host ALL DEPENDS ${_fwtool_host})
