# src/net -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-tcp-sync.c
    axl-tcp-async.c
    axl-net-wait.c
    axl-net-latency.c
    axl-udp.c
    axl-net-ping.c
    axl-net-sntp.c
    axl-net-arp.c
    axl-net-linkstats.c
    axl-net-resolve.c
    axl-net-interfaces.c
    axl-net-nic.c
    axl-net-addr.c
    axl-net-dhcp.c
    axl-net-driver-select.c
    axl-net-opts.c
    axl-net-legacy-ip4cfg.c
    axl-http-core.c
    axl-http-server.c
    axl-http-route.c
    axl-http-conn.c
    axl-http-request.c
    axl-http-dispatch.c
    axl-http-response.c
    axl-http-upload.c
    axl-http-ws.c
    axl-http-webdav.c
    axl-http-serve-fs.c
    axl-ssh-buf.c
    axl-ssh-packet.c
    axl-ssh-kex.c
    axl-ssh-crypt.c
    axl-ssh-auth.c
    axl-ssh-penalty.c
    axl-ssh-chan.c
    axl-ssh-rekey.c
    axl-ssh-idle.c
    axl-ssh-chan-codec.c
    axl-ssh-txq.c
    axl-ssh-server.c
    axl-ssh-host-key.c
    axl-ssh-host-key-load.c
    axl-http-client.c
    axl-http-client-async.c
    axl-tls.c
    axl-pk-verify.c
    axl-pk-mbedtls.c
    axl-pk-ed25519.c
    axl-ed25519-glue.c
    ${CMAKE_SOURCE_DIR}/deps/ed25519-ref10/ed25519.c
    axl-jose.c
    axl-crypto-rng.c
    axl-consttime.c
    axl-scram.c
    axl-aead.c
    axl-cipher.c
    axl-ecdh.c
    axl-url.c
    axl-inet-address.c
    axl-socket.c
    axl-socket-client.c
    axl-websocket.c)

# ed25519-ref10 is vendored for this module's AxlPk provider.
axl_source_includes_after(TARGET axl DIRS deps/ed25519-ref10 SOURCES
    ${CMAKE_SOURCE_DIR}/deps/ed25519-ref10/ed25519.c axl-ed25519-glue.c axl-pk-ed25519.c)

# The pre-UEFI-2.4 IPv4-config driver, which axl_net_legacy_ip4cfg_install()
# buffer-loads: an image of its own (the protocols it installs outlive the app
# that loads it) that links NO libaxl, because it is embedded IN libaxl -- as
# the blob axl_embedded_legacy_ip4cfg, libaxl's last member (deps/).
axl_objects(SRC axl-net-legacy-ip4cfg-drv.c)
axl_efi_image(axl_image_legacy_ip4cfg EFI build/axl-legacy-ip4cfg.efi
    SUBSYSTEM 11 NO_LIBS MODULE axl-legacy-ip4cfg
    OBJECTS ${AXL_CRT0_DRIVER} axl_obj_src_net_axl_net_legacy_ip4cfg_drv
    LINK_FLAGS --defsym=_AxlEntry=axl_legacy_ip4cfg_entry)
