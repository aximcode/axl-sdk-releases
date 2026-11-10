# AxlGoals.cmake -- the build's GOAL names, one target each, so a caller asks
# for a word (scripts/axl-build.sh ARCH=x64 <goal>) rather than a target name.
# Included last, by the root CMakeLists.txt. The names are the retired
# Makefile's, which every test and script still spells.
#
# A goal names the IMAGES it builds by their path under the build tree
# (`axl_image_target`, cmake/AxlImages.cmake, records which target makes
# each), or TARGETS, or a DIRECTORY's targets.
#
# Not here: `tool-sizes`, a REPORT over the `tools` goal (tools/), and `all`
# (CMake's own default target is everything).

axl_goal(9p IMAGES tools/9p.efi)
axl_goal(9p-mount-selftest IMAGES 9p-mount-selftest.efi)
axl_goal(9p-server-selftest IMAGES 9p-server-selftest.efi)
axl_goal(axbench IMAGES tools/axbench.efi)
axl_goal(axl-busybox IMAGES axl.efi build/axl-pack-stub-app-empty.efi
    build/axl-pack-stub-drv-empty.efi)
axl_goal(axl-pack-stub IMAGES build/axl-pack-stub-app-empty.efi
    build/axl-pack-stub-drv-empty.efi)
axl_goal(axlk-bootconfig-server IMAGES axlk-bootconfig-server.efi)
axl_goal(axlk-echo-server IMAGES axlk-echo-server.efi)
axl_goal(axlk-hwinfo-server IMAGES axlk-hwinfo-server.efi)
axl_goal(axlk-reqlog-server IMAGES axlk-reqlog-server.efi)
axl_goal(binding-driver IMAGES binding-driver.efi)
axl_goal(boot-path-selftest IMAGES boot-path-selftest.efi)
axl_goal(bss-probe IMAGES bss-probe.efi)
axl_goal(cancellable-demo IMAGES cancellable-demo.efi)
axl_goal(compositor-bench IMAGES compositor-bench.efi)
axl_goal(compositor-selftest IMAGES compositor-selftest.efi)
axl_goal(console-device-cycle-smoke IMAGES drivers/console-device-cycle-smoke.efi)
axl_goal(console-device-hold IMAGES console-device-present-probe.efi
    drivers/console-device-hold.efi)
axl_goal(console-device-hold-takeover IMAGES console-device-present-probe.efi
    drivers/console-device-hold-takeover.efi)
axl_goal(console-device-input-restore-smoke IMAGES
    drivers/console-device-input-restore-smoke.efi)
axl_goal(console-device-input-smoke IMAGES drivers/console-device-input-smoke.efi)
axl_goal(console-device-passthrough-smoke IMAGES
    drivers/console-device-passthrough-smoke.efi)
axl_goal(console-device-restore-smoke IMAGES drivers/console-device-restore-smoke.efi)
axl_goal(console-device-smoke IMAGES drivers/console-device-smoke.efi)
axl_goal(console-device-wide-restore-smoke IMAGES
    drivers/console-device-wide-restore-smoke.efi)
axl_goal(console-device-wide-smoke IMAGES drivers/console-device-wide-smoke.efi)
axl_goal(console-reshape-selftest IMAGES console-reshape-selftest.efi)
axl_goal(console-text-mode-selftest IMAGES console-text-mode-selftest.efi)
axl_goal(cpu-simd-selftest IMAGES cpu-simd-selftest.efi)
axl_goal(cpu-spin-fixture IMAGES cpu-spin-fixture.efi)
axl_goal(cpu-topology-selftest IMAGES cpu-topology-selftest.efi)
axl_goal(crashhandler IMAGES drivers/crashhandler.efi)
axl_goal(agent IMAGES tools/agent.efi)
axl_goal(crashtest IMAGES tools/crashtest.efi)
axl_goal(cursor-demo IMAGES cursor-demo.efi)
axl_goal(cursor-selftest IMAGES cursor-selftest.efi)
axl_goal(cxx-ctor-test IMAGES cxx-ctor-driver.efi cxx-ctor-fe-driver.efi
    cxx-ctor-fu-driver.efi cxx-ctor-sd-driver.efi cxx-ctor-svc-driver.efi
    cxx-ctor-test.efi)
axl_goal(cxx-json-selftest IMAGES cxx-json-selftest.efi)
axl_goal(cxx-seam-selftest IMAGES cxx-seam-selftest.efi)
axl_goal(cxx-streams-selftest IMAGES cxx-streams-selftest.efi)
axl_goal(devhandle-driver IMAGES devhandle-driver.efi)
axl_goal(driver IMAGES driver.efi)
axl_goal(driver-identity-test IMAGES driver-identity-test.efi)
axl_goal(driver-leak-test IMAGES driver-leak-test.efi)
axl_goal(driver-parent-leak-test IMAGES driver-parent-leak-test.efi
    stdio-bridge-driver.efi stdio-bridge-fix.efi)
axl_goal(echo-client IMAGES echo-client.efi)
axl_goal(echo-server IMAGES echo-server.efi)
axl_goal(echo-server-sync IMAGES echo-server-sync.efi)
axl_goal(embed-asset IMAGES embed-asset.efi)
axl_goal(event-demo IMAGES event-demo.efi)
axl_goal(exit-status-selftest IMAGES exit-status-selftest.efi)
axl_goal(exit-status-selftest-minimal IMAGES exit-status-selftest-minimal.efi)
axl_goal(shell-status-selftest IMAGES shell-status-selftest.efi)
axl_goal(timer-res-selftest IMAGES timer-res-selftest.efi)
axl_goal(net-latency-selftest IMAGES net-latency-selftest.efi)
axl_goal(fake-nic-driver-info IMAGES fake-nic-driver-info.efi)
axl_goal(fbcon IMAGES tools/fbcon.efi)
axl_goal(flushfail-fs-driver IMAGES flushfail-fs-driver.efi)
axl_goal(frame-anim-demo IMAGES frame-anim-demo.efi)
axl_goal(fs-path-selftest IMAGES fs-path-selftest.efi)
axl_goal(fs-read IMAGES fs-read-driver.efi fs-read-probe.efi)
axl_goal(gfx-avail-probe IMAGES gfx-avail-probe.efi)
axl_goal(gfx-demo IMAGES gfx-demo.efi)
axl_goal(gfx-mode-selftest IMAGES gfx-mode-selftest.efi)
axl_goal(gfx-present-bench IMAGES gfx-present-bench.efi)
axl_goal(gfx-present-selftest IMAGES gfx-present-selftest.efi)
axl_goal(gfx-simd-selftest IMAGES gfx-simd-selftest.efi)
axl_goal(gfx-window IMAGES gfx-window.efi)
axl_goal(heap-release-test IMAGES heap-rel-c.efi heap-rel-leak.efi heap-rel-sleep.efi
    heap-release-test.efi)
axl_goal(hello IMAGES hello.efi)
axl_goal(hello-minimal IMAGES hello-minimal-c.efi hello-minimal-cxx.efi)
axl_goal(http-plain-selftest IMAGES http-plain-selftest.efi)
axl_goal(http-server-driver IMAGES http-server-driver.efi)
axl_goal(image-path-test IMAGES image-path-driver.efi image-path-test.efi)
axl_goal(input-demo IMAGES input-demo.efi)
axl_goal(io-streams IMAGES io-streams.efi)
axl_goal(kbprobe IMAGES kbprobe.efi)
axl_goal(kbtune-drv IMAGES tools/kbtune-drv.efi)
axl_goal(kbtune-drv-test IMAGES kbtune-drv-test.efi)
axl_goal(kernel-poc IMAGES AxlKernelPoc.efi)
axl_goal(key-dump IMAGES key-dump.efi)
axl_goal(keytrace IMAGES keytrace.efi)
axl_goal(log-redirect-selftest IMAGES log-redirect-selftest.efi)
axl_goal(md5sum IMAGES tools/md5sum.efi)
axl_goal(minimal-log-off IMAGES minimal-log-off.efi)
axl_goal(minimal-log-on IMAGES minimal-log-on.efi)
axl_goal(net-gate-probe IMAGES net-gate-probe.efi refuse-unload-driver.efi)
axl_goal(pack-crashtest-w IMAGES pack-crashtest-w.efi)
axl_goal(pack-fixture IMAGES pack-fixture.efi)
axl_goal(pack-fixture-rt IMAGES pack-fixture-rt.efi)
axl_goal(pack-fixture-w IMAGES pack-driver-w.efi pack-fixture-w.efi)
axl_goal(pack-load IMAGES pack-kbtune-drv-w.efi pack-load-test.efi pack-sdsib-drv-w.efi)
axl_goal(pin-svc IMAGES pin-svc-driver-good.efi pin-svc-driver-shadow.efi
    pin-svc-launcher.efi)
axl_goal(pointer-demo IMAGES pointer-demo.efi)
axl_goal(pointer-tune-demo IMAGES pointer-tune-demo.efi)
axl_goal(radix-demo IMAGES radix-demo.efi)
# sdk: what scripts/install.sh stages from a build tree -- libaxl.a, the
# stand-in libc, the runtime and pack-stub objects, the host tools. The
# Makefile's `all` (its `sdk` goal named it); CMake's `all` is every image.
axl_goal(sdk TARGETS axl axl_standin axl_runtime_objects axl_pack_stub_objects
    axl_host_tools)
axl_goal(release-drivers IMAGES release-drivers/${AXL_ARCH}/NetworkCommon.efi
    release-drivers/${AXL_ARCH}/RamDiskDxe.efi release-drivers/${AXL_ARCH}/UsbCdcEcm.efi
    release-drivers/${AXL_ARCH}/UsbCdcNcm.efi release-drivers/${AXL_ARCH}/UsbRndis.efi)
axl_goal(reload-svc IMAGES reload-svc.efi)
axl_goal(reload-svc-dxe IMAGES reload-svc-dxe.efi)
axl_goal(reload-svc-fail-dxe IMAGES reload-svc-fail-dxe.efi)
axl_goal(reload-svc-stuck-dxe IMAGES reload-svc-stuck-dxe.efi)
axl_goal(ring-buf-demo IMAGES ring-buf-demo.efi)
axl_goal(runtime-demo IMAGES runtime-demo.efi)
axl_goal(sd-ergo IMAGES sd-ergo-driver.efi sd-ergo-launcher.efi)
axl_goal(sd-sibling IMAGES sd-sibling-driver-a.efi sd-sibling-driver-b.efi
    sd-sibling-probe.efi)
axl_goal(sd-sibling-driver-a IMAGES sd-sibling-driver-a.efi)
axl_goal(sd-sibling-driver-b IMAGES sd-sibling-driver-b.efi)
axl_goal(sd-sibling-probe IMAGES sd-sibling-probe.efi)
axl_goal(service-demo IMAGES service_demo-dxe.efi service_demo.efi)
axl_goal(service-demo-custom IMAGES service_demo_custom-dxe.efi service_demo_custom.efi)
axl_goal(sftp-server IMAGES tools/SftpServer.efi)
axl_goal(sha256sum IMAGES tools/sha256sum.efi)
axl_goal(shell-growth-test IMAGES shell-growth-test.efi)
axl_goal(shell-launcher IMAGES axl-shell-launcher.efi)
axl_goal(smbus-hc-shim IMAGES SmbusHcShim.efi)
axl_goal(ssh-server-selftest IMAGES ssh-server-selftest.efi)
axl_goal(sshd IMAGES tools/sshd.efi)
axl_goal(sshd-faults IMAGES sshd-t.efi)
axl_goal(sshd-full IMAGES tools/sshd-full.efi)
axl_goal(stderr-row IMAGES stderr-row.efi)
axl_goal(stderr-stale IMAGES stderr-stale.efi stderr_tick-dxe.efi)
axl_goal(stderr-tick IMAGES stderr_tick.efi)
axl_goal(stdio-bridge-fix IMAGES stdio-bridge-driver.efi stdio-bridge-fix.efi)
axl_goal(stdio-bridge-leak IMAGES stdio-bridge-leak.efi)
axl_goal(stdio-bridge-liveness-test IMAGES stdio-bridge-liveness-test.efi)
axl_goal(stdio-bridge-old IMAGES stdio-bridge-old.efi)
axl_goal(stdio-bridge-reap-test IMAGES stdio-bridge-leak.efi stdio-bridge-reap-test.efi)
axl_goal(stdio-bridge-self IMAGES stdio-bridge-self.efi)
axl_goal(svc-embonly IMAGES svc_embonly-decoy-dxe.efi svc_embonly-dxe.efi
    svc_embonly.efi)
axl_goal(svc-startfail IMAGES svc_startfail-dxe.efi svc_startfail.efi)
axl_goal(svc-tdfail IMAGES svc_tdfail.efi svc_tdfail_foreign.efi)
axl_goal(svc-verdict IMAGES svc_verdict.efi svc_verdict_leak.efi)
axl_goal(task-pool-mp-selftest IMAGES task-pool-mp-selftest.efi)
axl_goal(tcp-echo-server IMAGES tcp-echo-server.efi)
axl_goal(tests IMAGES AxlTest9p.efi AxlTestAta.efi AxlTestAuth.efi AxlTestCompositor.efi
    AxlTestCpuIdle.efi AxlTestCrypto.efi AxlTestCursor.efi AxlTestData.efi
    AxlTestDriver.efi AxlTestEd25519.efi AxlTestEvent.efi AxlTestFileView.efi
    AxlTestFind.efi AxlTestFsProvider.efi AxlTestFw.efi AxlTestGfx.efi
    AxlTestGfxRegion.efi AxlTestHii.efi AxlTestIO.efi AxlTestInput.efi AxlTestIpmi.efi
    AxlTestJose.efi AxlTestJsonConformance.efi AxlTestJsonCorpus.efi AxlTestLog.efi
    AxlTestLoop.efi AxlTestMath.efi AxlTestMem.efi AxlTestNet.efi AxlTestNvme.efi
    AxlTestPack.efi AxlTestPieceTree.efi AxlTestPixmap.efi AxlTestPlatform.efi
    AxlTestReplace.efi AxlTestRuntime.efi AxlTestScsi.efi AxlTestSmart.efi
    AxlTestSmbus.efi AxlTestSsh.efi AxlTestSshHostKey.efi AxlTestSshHostKeyWeak.efi
    AxlTestSshdBind.efi AxlTestSshdConf.efi AxlTestSshdSftp.efi AxlTestString.efi
    AxlTestTask.efi AxlTestTruetype.efi AxlTestUtil.efi AxlTestVterm.efi AxlTestXml.efi
    axl-shell-launcher.efi build/axl-pack-stub-app-empty.efi
    build/axl-pack-stub-drv-empty.efi)
axl_goal(time-settime-selftest IMAGES time-settime-selftest.efi)
axl_goal(tools IMAGES build/axl-pack-stub-app-empty.efi
    build/axl-pack-stub-drv-empty.efi drivers/crashhandler.efi tools/9p.efi
    tools/SftpServer.efi tools/agent.efi tools/ata.efi tools/axbench.efi tools/cat.efi tools/clip.efi
    tools/crashtest.efi tools/cut.efi tools/dmidecode.efi tools/fbcon.efi
    tools/fetch.efi tools/find.efi tools/fwtool.efi tools/grep.efi tools/hexdump.efi
    tools/i2c.efi tools/ipmi.efi tools/kbtune-drv.efi tools/kbtune.efi tools/lsacpi.efi
    tools/lspci.efi tools/lsproto.efi tools/lsusb.efi tools/md5sum.efi tools/memspd.efi
    tools/mkfixture.efi tools/mkrd.efi tools/netinfo.efi tools/netload.efi
    tools/nvme.efi tools/paste.efi tools/rfbrowse.efi tools/rndisfix.efi tools/scsi.efi
    tools/sed.efi tools/sha256sum.efi tools/smart.efi tools/sshd-full.efi tools/sshd.efi
    tools/sysinfo.efi tools/tar.efi tools/timetest.efi tools/tr.efi)
axl_goal(upgrade-svc IMAGES upgrade-svc-dxe.efi upgrade-svc.efi)
axl_goal(upgrade-svc-test IMAGES upgrade-svc-dxe-broken-packed.efi
    upgrade-svc-dxe-broken.efi upgrade-svc-dxe-broken2.efi upgrade-svc-dxe-g10.efi
    upgrade-svc-dxe-g8.efi upgrade-svc-dxe-g9.efi upgrade-svc-dxe-hang.efi
    upgrade-svc-dxe-q1.efi upgrade-svc-dxe-q2.efi upgrade-svc-dxe-q5.efi
    upgrade-svc-dxe-v2.efi upgrade-svc-dxe-wedge.efi upgrade-svc-dxe.efi
    upgrade-svc-t.efi upgrade-svc-t1.efi upgrade-svc.efi)
axl_goal(vmedia IMAGES image-path-driver-big.efi vmedia-check.efi vmedia-fs-driver.efi)
axl_goal(volume-map-test IMAGES volume-map-test.efi)

# Host tools: what runs on the build machine.
axl_goal(axl-efi-pack TARGETS axl_host_tools)
axl_goal(fwtool-host TARGETS axl_fwtool_host)
# Every fixture this directory declares. The Makefile's goal of this name
# compiled the fixture OBJECTS only (it was the lint database's input); every
# image built from them is a superset, which is what a caller asking for the
# fixtures wants.
axl_goal(integration-fixtures DIRECTORY test/integration)
# sshd-rtk / sshd-full-rtk are goals of tools/ (they report and copy, as
# the Makefile's did).

axl_goals_finalize()

# NON_ARTIFACT_GOALS (scripts/axl-lists.conf): what the integration runner
# must never record as pre-built. A name that is no target here would be a
# list nobody checks -- the goal renamed, the runner then skipping its report.
axl_list(_axl_non_artifact NON_ARTIFACT_GOALS)
foreach(_g IN LISTS _axl_non_artifact)
    if(NOT TARGET ${_g})
        message(FATAL_ERROR "scripts/axl-lists.conf: NON_ARTIFACT_GOALS names ${_g}, "
            "which is no target of this build")
    endif()
endforeach()
