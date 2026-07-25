# AXL Public-API Consistency Audit — 2026-07-19
_Read-only audit of all 145 `include/axl/*.h` headers by a 21-agent sweep (129 raw findings, consolidated). Recommendations are majority-rule across the corpus. This is a prioritization catalog — NOTHING here is applied. See `ROADMAP.md` (Open backlog -> API hygiene). A completeness critic flagged second-pass gaps (bottom); treat those as TODO before calling this exhaustive._
# AXL Public-API Consistency Catalog

Consolidated from 129 raw audit findings (dupes merged, non-inconsistencies dropped).
Canonical forms are majority-rule across `include/axl/*.h`. Verified against the tree:
`AxlStatus` is a real return type in the **net/event/wait** cluster
(tcp, udp, net, socket, event, wait, cancellable, http-client) and absent
elsewhere; 47 types register `AXL_DEFINE_AUTOPTR_CLEANUP` while the flagged
ones register none.

Categories ranked by impact x reach.

---

## 1. return-type  — HIGH  (~45 symbols, blast ~450)

Three distinct sub-splits, all pointing one way:

**Canonical:**
- Multi-outcome (3+ branched outcomes) -> typed `AxlStatus` / `Axl<Module>Status`
  (as net/event/wait already do). Two outcomes -> plain `int` AXL_OK/AXL_ERR.
  Pure yes/no -> `bool`. `Axl<Module>Status` members number `_OK=0` then **negative**.
- Any fallible op with a paired teardown, a filled out-param, or a security
  verdict -> `AXL_WARN_UNUSED`.
- A "look something up / parse into out-param" op -> `int` status, **not** `bool`.

### 1a. int (or ad-hoc #define / magic literal) that should be AxlStatus
| symbol | header | callers | note |
|---|---|---|---|
| axl_tpm_seal/_unseal | axl-tpm.h | 13 | already emits OK/INVALID/ERR/DENIED thru int |
| axl_tls_handshake/_read | axl-tls.h | 10 | magic 0/1/-1; "1"=more-data isn't in AxlStatus |
| axl_net_ensure_drivers | axl-net.h | 11 | ad-hoc #define OK/NOT_FOUND/NO_LINK |
| axl_driver_load_sibling/_ensure/_ensure_with_embedded | axl-driver.h | 21 | returns 3-4 real AxlStatus members |
| axl_http_get/_post/_put/_delete/_request | axl-http-client.h | 9 | sync core computes AxlStatus, collapses to ERR |
| axl_udp_send | axl-udp.h | 7 | leaks AXL_CANCELLED(-2) thru undocumented int |
| axl_loop_iterate_until | axl-loop.h | 18 | mixes named AXL_CANCELLED + bare 0/-1 |
| axl_spd_read/_dump_raw/_decode | axl-spd.h | 4 | empty vs unsupported vs I/O |
| axl_attempt_recover | axl-attempt.h | 2 | tri-state 1/0/-1 magic numbers |
| axl_ramdisk_create/_register_image | axl-ramdisk.h | 17 | 4-5 causes -> one ERR |
| axl_pbkdf2_hmac_sha256 | axl-digest.h | 8 | OK/INVALID/ERR |
| axl_fv_find_file_name | axl-fv.h | 7 | caller already branches on NOT_FOUND |
| axl_volume_set_map/_unmap | axl-fs.h | 5 | OK/ERR/UNSUPPORTED |
| axl_path_search | axl-path.h | 6 | OK/NOT_FOUND/ERR |
| axl_tcp_accept/_connect_timeout | axl-tcp.h | 11 | OK/TIMEOUT/CANCELLED/ERR (low) |
| axl_9p_connect (+read/write/list/mkdir/remove/rename) | axl-9p.h | 2 | version-reject vs unreachable vs attach |
| axl_app_boot_path | axl-app.h | 8 | low |
| **AxlFsStatus** (enum itself) | axl-fs-provider.h | 28 | **positive-numbered**, violates negative convention |

### 1b. bool that should be int/AxlStatus (parse/lookup shape)
| symbol | header | callers |
|---|---|---|
| axl_json_* (get_string + 12 fns, whole surface) | axl-json.h | 45 |
| axl_http_request_get_json | axl-http-request | 7 |
| axl_hash_table_add (-> AxlHashTableInsertResult like insert/replace) | axl-hash-table.h | 6 |

### 1c. int that should be bool (only two outcomes)
| axl_diag_probe_protocol | axl-diag.h | 2 |

### 1d. missing AXL_WARN_UNUSED (security / must-check / paired-teardown)
| symbol | header | callers | note |
|---|---|---|---|
| axl_pk_verify/_key_verify/_aead_open/_aead_seal | axl-crypto.h | 25 | signature/tag verdict silently droppable |
| axl_jws_verify/_jwt_verify/_jws_sign | axl-jose.h | 35 | fail-closed token verify; whole header unmarked |
| axl_tcp_connect/_listen/_accept (+_via/_timeout) | axl-tcp.h | 21 | style doc's textbook case; *out_sock stale |
| axl_rng_bytes | axl-rng.h | 10 | nonce/key buffer left unfilled |
| axl_file_writer_close | axl-fs.h | 9 | docstring says MUST check; sibling _write is marked |
| axl_console_read_key | axl-console.h | 10 | 6/7 siblings marked |
| axl_attempt_is_quarantined | axl-attempt.h | 11 | fail-closed predicate; sibling is_cancelled marked |
| axl_scsi_inquiry/_read_capacity/_health, axl_serial_get_mode/_control, axl_shell_sources | axl-scsi/serial/shell.h | 3 | storage/serial/shell trio all skip it |
| axl_nvstore_* (8 fns) | axl-nvstore.h | 13 | low |
| axl_pci_get_* / read-get family | axl-pci.h | 10 | low |
| axl_setenv/_unsetenv | axl-env.h | 21 | low |

### 1e. void that should carry a failure channel
| symbol | header | callers | note |
|---|---|---|---|
| axl_gfx_region_union (+_subtract/_intersect/_copy/*_rect) | axl-gfx-region.h | 51 | OOM-degrade vs bad-arg only via side-channel is_lossy(); wants status enum |
| axl_http_response_set_streamer/_json/_text/_static/_bytes/_range/_content_range | axl-http-response | 9 | NULL-streamer precondition unreportable |
| axl_socket_address_to_ipv4 | axl-socket.h | 3 | sibling ws_conn_peer returns int |
| axl_mem_get_stats | axl-mem.h | 16 | sibling mem_phys_get_policy returns int |

---

## 2. ownership (C++ RAII autoptr)  — HIGH  (4 symbols, blast ~279)

**APPLIED (Batch B expanded):** `AXL_DEFINE_AUTOPTR_CLEANUP` registered for
`AxlGfxBuffer`, `AxlGfxPath`, `Axl9pClient` (the 3 opaque-handle types below)
plus the 7 the completeness-critic surfaced: `AxlGfxGradient`,
`AxlGfxDisplayList`, `AxlCursor`, `AxlConsoleTerm`, `AxlFwImage`,
`AxlTarWriter`, `AxlTarReader`. Purely additive; RAII tests pin scope-exit
cleanup via live-alloc count (both arches). The `axl_smbios_get_string`
static-buffer aliasing item (below) is NOT an autoptr gap — deferred to a
naming/signature batch.

**Canonical:** every opaque `_new`/`_connect`/`_open`+`_free`/`_disconnect`
resource registers `#ifdef AXL_HAVE_AUTOPTR / AXL_DEFINE_AUTOPTR_CLEANUP(T, teardown) / #endif`
right after its teardown decl. Verified: 47 types do; these do not.

| symbol | header | callers | note |
|---|---|---|---|
| **AxlGfxBuffer** | axl-gfx-surface.h | 218 | most-used opaque type here; RAII is a headline design goal |
| AxlGfxPath | axl-gfx-path.h | 42 | sibling AxlGfxRegion registers it |
| Axl9pClient | axl-9p.h | 15 | every sibling net type (tcp/udp/socket_client) registers it |
| axl_smbios_get_string (static-buffer aliasing) | axl-smbios.h | 4 | returns shared `static` buf, non-reentrant vs get_string_utf8; rename `_static` or take caller buf |

---

## 3. param-order  — HIGH  (~14 symbols, blast ~114)

**Canonical (style doc, explicit):** out / in-out params come **last**;
buffer before length; opaque `user`/`ctx` cookie is the final param; a paired
constructor/destructor mirror each other's argument order. `(out, cap, *count)`
for list-into-buffer.

| symbol | header | callers | violation |
|---|---|---|---|
| axl_compress | axl-compress.h | 12 | scalar `level` trails out/out_len |
| axl_console_device_install | axl-console-device.h | 6 | out-handle FIRST |
| axl_console_mirror_install | axl-console-mirror.h | 10 | out-handle FIRST |
| axl_protocol_register | axl-sys.h | 8 | handle LAST here, FIRST in _register_multiple/_unregister siblings |
| axl_shared_driver_publish/_unpublish | axl-shared-driver.h | 8 | handle/iface **swapped** between pair; both void*, silent miscompile |
| axl_cpu_topology | axl-cpu.h | 12 | two outs precede input cap, third out trails it |
| AxlByteReader.read (fnptr) | axl-find.h | 10 | len before buf; 4+ implementers |
| axl_mem_phys_read_range / axl_io_read_range | axl-mem-phys/region.h | 8 | [out] buf before trailing input access_width |
| AxlPageFillFunc | axl-page-cache.h | 5 | `user` FIRST |
| AxlTaskProc / AxlTaskComplete | axl-task.h | 11 | `arg` before `arena` |
| axl_udp_sendrecv | axl-udp.h | 3 | timeout_ms trails 3 out-params |
| axl_service_detach_driver | axl-service.h | 8 | speculative `svc` param, docstring says unused — **WON'T-FIX: deliberately reserved + kept for source-compat, documented** |
| AxlUploadHandler | axl-http-server.h | 6 | `aborted` after `data` (low) — **DEFERRED: the data-before-aborted shape is shared with the webdav write-close callback; a cookie-last pass should reorder both together** |
| axl_tls_write_async | axl-tls.h | 7 | missing AxlCancellable that every tcp _async has (low) — **DEFERRED: this is a missing param (a real cancellation FEATURE to wire + honor), not a reorder; belongs in an async-consistency pass** |

---

## 4. result-passing  — MEDIUM-HIGH  (~11 symbols, blast ~141)

**Canonical:** a "make me an object" constructor returns `T*` directly (NULL on
failure) unless it has a genuine 3rd outcome (then `Axl<Module>Status`). Sibling
`_get`/parse families keep one shape.

| symbol | header | callers | note |
|---|---|---|---|
| axl_tree_insert/_replace | axl-tree.h | 24 | void; swallows OOM; peer hash_table_insert returns result enum |
| axl_console_screen_new | axl-console-screen.h | 22 | int+out; 48/50 `_new` return T* |
| axl_hex_parse_u64 | axl-str.h | 17 | returns digit-count (min 1), inverts `!=0`==ERR idiom of file |
| axl_config_get_bool/_int/_uint/_get | axl-config.h | 16 | no `def` param; counterpart axl-config-file.h has one |
| axl_vterm_new | axl-vterm.h | 14 | int+out for 2-outcome constructor |
| axl_udp_open/_open_via | axl-udp.h | 13 | int+out (net sub-convention; consistent internally) |
| axl_smbios_copy_string_utf8 | axl-smbios.h | 10 | return 0 = both fail AND empty-string |
| axl_net_arp_list | axl-net.h | 6 | (out,cap,count) vs sibling list_interfaces' (out,*count) |
| axl_tls_accept/_connect | axl-tls.h | 5 | pointer-return vs tcp_accept's int+out |
| axl_device_path_make_vendor | axl-device-path.h | 4 | int+out AND `_make` verb; both one-offs |
| axl_args_get_uint_offset | axl-args.h | 6 | **no change** — self-documented, 0 is valid value |

---

## 5. naming  — MEDIUM  (~24 symbols, blast ~370, mostly cosmetic; 2 real prefix splits)

**Canonical:** `axl_<module>_<verb>[_<noun>]`; getters that return one borrowed
value drop `get_`; getters that fill an out-param keep it; boolean predicates use
`is_`; one module prefix per header matching the filename; spell concepts the same
on both halves of a reader/writer pair.

| symbol | header | callers | note |
|---|---|---|---|
| axl_storage_* vs axl_smart_* | axl-smart.h | 22 | **two prefixes, one module**; tools depend on both |
| axl_sntp_query / AxlSntpResult | axl-net.h | 9 | drops net_/AxlNet prefix every sibling carries |
| axl_rb_insert/_erase/_first/... vs axl_rb_tree_init/_empty | axl-rb-tree.h | 52 | two prefixes for one type (kernel-rbtree echo?) |
| axl_xml_writer_start_element/_end_element | axl-xml.h | 47 | verb_noun; JSON twin uses noun_verb (_begin/_end) |
| axl_smbios_slot_*_str vs axl_smbus_transport_string | axl-smbios/smbus.h | 48 | `_str` vs `_string` for same shape (project-wide split) |
| axl_input_attach_/detach_* | axl-input.h | 28 | header says it wraps loop add_/remove_ |
| axl_boot_option_get/_set/... | axl-boot.h | 25 | noun_verb; corpus is verb_noun (~220 hits) |
| axl_args_user_data/_program_name | axl-args.h | 19 | drop get_; 9 siblings keep it |
| axl_queue_get_length / axl_ring_buf_get_length | axl-queue/ring-buf.h | 16 | drop get_ (bare-noun count accessor is dominant) |
| axl_protocol_guid_name vs axl_net_protocol_name | axl-driver-info.h | 14 | same op, reversed noun order |
| axl_clock_gettime/_getres | axl-time.h | 14 | POSIX echo (deliberate) |
| axl_xml_reader_attr vs writer_attribute | axl-xml.h | 11 | abbrev split in same pair |
| axl_compute_hmac | axl-hmac.h | 9 | verb-first (GLib echo; systemic w/ compute_checksum) |
| axl_http_server_serve_fs | axl-http-server.h | 9 | 10 sibling registrations are add_* |
| CrashRecordHeader / CRASH_* (no Axl/AXL_) | axl-crashrecord.h | 9 | whole header unprefixed |
| axl_alloc_pages/_free_pages | axl-mem.h | 8 | drop mem_; siblings carry it |
| AxlHexDumpLog (PascalCase macro+params) | axl-hexdump.h | 0 | **only PascalCase macro in 145 headers; free fix** |
| axl_runtime.h symbols (axl_efi_*/axl_yield) | axl-runtime.h | 6 | header publishes zero self-prefixed symbols |
| axl_net_last_config_method | axl-net.h | 4 | drop-get_ getter that returns value directly |
| axl_rb_tree_empty | axl-rb-tree.h | 3 | missing is_ (only bool-empty predicate that does) |
| axl_reset/_map_refresh vs axl_sys_* | axl-sys.h | 2 | 2-and-2 split on sys_ segment |
| axl_xml_writer_textn | axl-xml.h | 2 | glued verb+count; corpus uses `_len` |
| axl_scsi_passthru `data_transferred` | axl-scsi.h | 1 | sibling serial uses out_written/out_read |

---

## 6. constructor-naming  — MEDIUM  (3 symbols, blast ~115)

**Canonical:** `_new` / `_free` (53 vs 2 `_create`; 66 vs 2 `_destroy`).

| symbol | header | callers | note |
|---|---|---|---|
| axl_surface_create/_destroy | axl-compositor.h | 85 | same file's axl_compositor_new/_free is canonical |
| axl_pk_keygen | axl-crypto.h | 20 | sibling axl_ecdh_new does identical op as `_new` |
| axl_virtual_pointer_install | axl-input/pointer | 10 | install+out-param (low; publishes real protocol) |

---

## 7. teardown-naming  — MEDIUM  (7 symbols, blast ~187; 1 real bug, rest cosmetic/deliberate)

**Canonical:** `_open`->`_close`, `_new`->`_free` (void, NULL-safe); loop
attachments use one `AxlSourceId` + `axl_loop_remove_source`; heap+embedded types
expose `_init`/`_deinit`.

| symbol | header | callers | note |
|---|---|---|---|
| axl_queue_free / axl_queue_init | axl-queue.h | 5 | ~~**BUG**: _free does axl_free(struct) — corrupts stack-init'd queue~~ **FIXED (Batch D)**: added axl_queue_deinit/_deinit_full (mirrors axl_ring_buf_deinit); _free/_free_full docstrings redirect stack users |
| axl_defer / axl_defer_cancel | axl-defer.h | 25 | parallel uint32 handle + _cancel vs loop's AxlSourceId+remove_source |
| axl_log_ring_attach | axl-log.h | 7 | no public detach; 3 verb pairs (add/remove, attach, attach/detach) in one header |
| axl_piece_tree_free | axl-piece-tree.h | 63 | `_open` constructor but `_free` teardown (10/10 others pair _open/_close); has _new too |
| axl_image_unload | axl-image.h | 7 | int-returning (deliberate UEFI mirror); document it |
| axl_socket_free / axl_tcp_close | axl-socket/tcp.h | 80 | mode param (deliberate abortive teardown; no change) |

---

## 8. callback-status  — LOW-MEDIUM  (7 symbols, blast ~52; mostly documented)

**Canonical:** loop/source-style bool callback: `true`=continue/keep-armed.
Live/hardware enumeration walk: `int` 0=continue, non-zero=stop-and-propagate,
-1=hard error (axl_dir_walk / pci_tree / usb_tree / image_enumerate / nvstore_iter).
Handler success/fail: `int` AXL_OK/AXL_ERR.

| symbol | header | callers | note |
|---|---|---|---|
| AxlFvFileFn | axl-fv.h | 3 | bool-stop; domain family uses int-propagate |
| AxlTreeForeachFunc | axl-tree.h | 6 | bool true=STOP inverts loop polarity |
| AxlNTreeTraverseFunc | axl-ntree.h | 10 | bool true=STOP (deliberate GLib parity) |
| AxlUsbTreeFn | axl-usb.h | 7 | -1 error sentinel collides with callback stop-code |
| AxlImageIterFn | axl-image.h | 6 | int 0=continue reuses AXL_OK's type (documented) |
| AxlConsoleOps::scrollrect/set_term_prop | axl-console-ops.h | 6 | int 1/0 vs bool (documented, deliberate) |
| AxlInputCallback | axl-input.h | 14 | bool continue/remove (documented, deliberate) |

---

## 9. enum-flag  — LOW  (6 symbols, blast ~18)

**Canonical:** closed value/flag sets are `typedef enum { ... } Axl<Module>Kind/Flags`,
used as the field/param type (even when flags widen to uint32_t).

| symbol | header | callers | note |
|---|---|---|---|
| AXL_FONT_MONOSPACE/_VARIABLE | axl-font.h | 9 | bare #define bitmask, no enum wrapper |
| AxlWsHandler event (size_t + #define AXL_WS_*) | axl-http-server.h | 3 | plain 0..3 discriminator, not bitflags -> enum |
| AXL_RESET_COLD/_WARM/_SHUTDOWN | axl-sys.h | 2 | bare int; no POSIX ancestor (unlike AXL_SEEK_*) |
| AXL_CPU_ARCH_X64/_AA64 | axl-cpu.h | 3 | bare int beside a properly-enum'd `kind` in same struct |
| AXL_CFG_BOOL/INT/... | axl-config.h | 0 | bare #define kind tag |
| AxlShellSource values (no type qualifier) | axl-shell.h | 1 | sibling AxlShellKind qualifies its values |

---

## 10. param-types  — LOW  (1 family, blast ~114)

**Canonical:** stdint types (`uint8_t`/`uint16_t`), never raw `char`/`short`.

| axl_smbios_find/_find_next/_get_string/_version (unsigned char / unsigned short*) | axl-smbios.h | 114 | newer fns in same header already use uint8_t/uint16_t |

---

## 11. const-correctness  — LOW  (2 symbols, blast ~39)

**Canonical:** read-only accessors const-qualify the subject.

| axl_string_len | axl-string.h | 33 | non-const; sibling axl_string_str is const |
| axl_ring_buf_is_empty/_full/_get_length/_peek... | axl-ring-buf.h | 6 | non-const while pushes_total/_lost are const |

---

## 12. struct-exposure  — LOW  (blast 0)

`AxlQueue` header says "exposed for direct access"; `AxlRingBuf`/`AxlRBNode` (same
role, embeddable) say "private/opaque". Pick one contract for embeddable public
structs; AxlQueue is the 1-of-3 outlier.

---

# Recommended batches (priority order)

**Batch A — AxlStatus promotion + security WARN_UNUSED (HIGH, ~1-2 days).**
The single highest-leverage split. Two moves:
(1) Mark `AXL_WARN_UNUSED` on the security/must-check set — crypto (`axl_pk_verify`,
`_aead_open/_seal`), jose (`axl_jws_verify`, `_jwt_verify`), tcp connect/listen/accept,
`axl_rng_bytes`, `axl_file_writer_close`. Pure additive, compile-time only, catches
real fail-open bugs.
(2) Promote the functions that already emit AxlStatus members through `int`
(`axl_tpm_seal/_unseal`, `axl_driver_load_sibling/_ensure*`, `axl_http_*` sync,
`axl_udp_send` leak fix, `axl_loop_iterate_until`) to `AxlStatus`, and give
`axl_net_ensure_drivers`/`axl_tls_*`/`axl_ramdisk_*`/`axl_spd_read` a
`Axl<Module>Status`. Also renumber `AxlFsStatus` to the negative convention.
`AxlStatus` return is a no-op ABI change (it's `int`), so callers keep compiling.

- ✅ **A(1) security set DONE:** `AXL_WARN_UNUSED` marked on `axl_pk_verify` /
  `_key_verify` / `_aead_seal` / `_aead_open` (crypto), `axl_jws_sign` / `_verify` /
  `axl_jwt_verify` (jose), `axl_tcp_connect` / `_via` / `_timeout` / `_listen` /
  `_accept`, `axl_rng_bytes`, `axl_file_writer_close`. The mark surfaced 6 real
  ignored-return sites — the headline being `http-serve-fs` silently dropping a
  failed upload final-flush (now checked + warned); jose tests + io tests now
  assert `== AXL_OK`; axbench's void sink-callback captures write errors into a
  flag reported at finalize (no `(void)` casts — the must-check returns are
  actually checked, per the "proper fix over punt" rule). Also marked the
  `axl_tcp_listen_via` sibling. Both arches 7959/0 non-TLS + jose 86/crypto 90
  under AXL_TLS=1.
- ✅ **A(1) must-check tail DONE (split scope):** marked `AXL_WARN_UNUSED` on
  `axl_console_read_key`, `axl_attempt_is_quarantined`, `axl_scsi_inquiry`/
  `_read_capacity`/`_health`, `axl_serial_get_mode`/`_get_control`,
  `axl_shell_sources`, and the pci out-param fillers `axl_pci_get_vid_did`/
  `_get_class_code`/`_get_header_type`/`_get_subsystem`/`_bridge_info`/`_vpd_read`.
  Fixed the surfaced ignored returns with real checks (NOT `(void)`-casts):
  mkfixture skips functions whose config space can't be read; lspci/netinfo
  default to unknown (class 0 / 0000:0000) on a failed read; the shell-coexist
  test-app reports no sources on query failure. **REVERTED after review** (the
  audit-flagged "larger ignored-return surface" was real — ~30 sites, mostly
  best-effort): `axl_nvstore_*` (its `get*` has a size-query idiom where the
  return is intentionally an error and `size` is the real signal; read sites use
  pre-zeroed buffers) and `axl_setenv`/`_unsetenv` (crash-handler persistence,
  cleanup deletes, test setup — legitimately fire-and-forget). 7974/0 both arches.
- ✅ **A(2a) clean AxlStatus promotions DONE:** `int` -> `AxlStatus` on the
  functions that already return 3+ AxlStatus-valued outcomes —
  `axl_tpm_seal`/`_unseal`, `axl_driver_load_sibling`/`_ensure`/`_ensure_with_embedded`,
  `axl_udp_send` (documents its AXL_CANCELLED leak), `axl_loop_iterate_until`.
  Source-compatible / no-op ABI (enum is int-compatible), EXCEPT `loop_iterate_until`
  timeout, which was the imprecise `-1` and is now properly `AXL_TIMEOUT` (the one
  value change; only 1 in-tree test checked it). Also dropped 4 noise `(void)` casts
  on the non-nodiscard `iterate_until`. 7959/0 both arches.
- ✅ **A(2a) module-status enums DONE** (2 commits):
  - **Part 1 (value-preserving typed wraps, `c6886a67`):** `axl_net_ensure_drivers`
    #define trio (0/-1/-2) -> typed enum `AxlNetDriversStatus`; `axl_tls_handshake`/
    `_handshake_async`/`_read` magic 0/1/-1 -> `AxlTlsStatus` (`AXL_TLS_OK=0`,
    positive `AXL_TLS_WANT_MORE=1` — the one non-error state AxlStatus can't
    express, mirrors OpenSSL WANT_READ — `AXL_TLS_ERR=-1`). Also fixed rndisfix.c's
    sloppy `!= AXL_OK` -> `!= AXL_NET_DRIVERS_OK`.
  - **Part 2 (surface richer AxlStatus, this commit):** the catalog assumed NEW
    module enums were needed, but `AxlStatus` is already rich enough (INVALID/
    NOT_FOUND/UNSUPPORTED/NO_RESOURCES/IO_ERROR/TIMEOUT/CANCELLED/...), so
    `axl_http_get`/`_post`/`_put`/`_delete`/`_request`, `axl_ramdisk_create`/
    `_register_image`, and `axl_spd_read`/`_dump_raw`/`_decode` were promoted
    `int` -> `AxlStatus`, returning the specific cause instead of collapsing to
    `AXL_ERR` (http surfaces the sync core's computed `r.st`; ramdisk/spd map each
    failure path: bad-args=INVALID, no-protocol/controller=UNSUPPORTED, empty
    slot=NOT_FOUND, OOM=NO_RESOURCES, firmware Register fail=IO_ERROR). Test-first
    on the unit-testable INVALID/UNSUPPORTED paths; the deeper HW paths are
    firmware-gated. Both arches 7974/0. Independent review CONFIRMED-CLEAN (caught a
    stale `axl_spd_read` doc + a spd/ramdisk transport-absent mapping inconsistency
    — both fixed: no-controller is now UNSUPPORTED in both).
- ✅ **A(2b) `AxlFsStatus` renumber DONE:** enum renumbered positive `1..12` ->
  negative `-1..-12` (`AXL_FS_OK` stays 0), matching the `Axl<Module>Status`
  negative convention. Source-compatible: every in-tree/consumer use is a named
  constant, `status_to_efi` is a named-case switch (renumber-safe), and the value
  never crosses a wire boundary (translated to `EFI_STATUS` first). Added a
  test pinning the exact negative values (RED-confirmed against the old numbering);
  independent whole-tree review CONFIRMED-SAFE. 7972/0 both arches. Only consumer
  = axl-webfs (68 named-constant sites) — recompile only, zero manual edits.

**Batch B — C++ RAII autoptr gap (HIGH, ~1 hour). ✅ DONE (expanded).**
Added the `AXL_HAVE_AUTOPTR`-guarded `AXL_DEFINE_AUTOPTR_CLEANUP` block for
`AxlGfxBuffer` (218 callers), `AxlGfxPath`, `Axl9pClient`, and the 7 the
completeness-critic surfaced (`AxlGfxGradient`, `AxlGfxDisplayList`, `AxlCursor`,
`AxlConsoleTerm`, `AxlFwImage`, `AxlTarWriter`, `AxlTarReader`). Purely additive;
closed the headline-feature hole. The `axl_smbios_get_string` -> `_static` rename
is a naming/signature change, not additive — deferred (goes with Batch E/naming).

**Batch C — param-order safety fixes (HIGH, ~half day).**
Reorder the out-params-last violators, prioritizing the silent-miscall hazards:
`axl_shared_driver_publish/_unpublish` (swapped void* handle/iface),
`axl_protocol_register` (handle position), `axl_compress` (level after out),
`axl_console_device_install`/`_mirror_install` (out first). These are signature
breaks — do them together, sweep the in-tree callers, one commit.
- ✅ **DONE:** flagship `axl_shared_driver_unpublish` silent-swap (`4dc72d43`);
  `axl_compress` (level before out), `axl_console_device_install` /
  `_mirror_install` (out last), `axl_protocol_unregister` (mirror register:
  name/iface/handle), `axl_mem_phys_read_range` / `axl_io_read_range`
  (access_width before out buf), `axl_udp_sendrecv` (timeout before rx outs).
- ✅ **DONE (callback fnptrs):** `AxlByteReader.read` (buffer before length),
  `AxlPageFillFunc` (user cookie last), `AxlTaskProc` / `AxlTaskComplete`
  (arena before the arg cookie) — all implementers + framework invocation sites
  swept; compiler-caught reorders, both arches green.
- **Left as-is (reviewed):** `axl_protocol_register` is already canonical
  (in/out handle last); `axl_register_multiple` is forced handle-first by its
  varargs. `axl_cpu_topology` — current `(total, enabled, out, out_cap, out_n)`
  is defensible: all four outs are optional (count-only callers pass out=NULL),
  and it keeps the canonical `(out, cap, *count)` triple adjacent; reordering 11
  callers for a debatable gain isn't worth it.

**Batch D — constructor/result-passing normalization (MEDIUM, ~half day).**
Rename `axl_surface_create/_destroy` -> `_new/_free` (85 callers, in-header
inconsistency), `axl_pk_keygen` -> `axl_pk_key_new`, `axl_device_path_make_vendor`
-> `axl_device_path_new_vendor`. Convert 2-outcome int+out constructors
(`axl_vterm_new`, `axl_console_screen_new`) to direct `T*` return. Fix the
`axl_queue_free` stack-corruption bug (add `axl_queue_deinit`) — that one is a real
defect, not just a naming nit, do it here.
- ✅ **DONE (whole batch):** `axl_queue_free` bug (`520da7ed`); the three
  constructor renames — `axl_surface_new/_free`, `axl_pk_key_new`,
  `axl_device_path_new_vendor` (`4d3a9020`); and the int+out -> `T*` conversion of
  `axl_vterm_new` / `axl_console_screen_new` (drops the `**out` param, returns the
  handle or NULL).

**Batch E — naming prefix splits (MEDIUM, ~half day). ✅ DONE.**
The two real ones first: `axl_storage_*` vs `axl_smart_*` (split axl-storage.h or
unify prefix) and `axl_sntp_query`/`AxlSntpResult` -> `axl_net_sntp_query`. Then the
free/cheap wins: `AxlHexDumpLog` -> `axl_hexdump_log` (0 callers), drop `get_` from
count accessors and `is_`-add to `axl_rb_tree_empty`, prefix `CrashRecord*`/`CRASH_*`.

- ✅ **E1 storage/smart split:** chose SPLIT (not unify) — moved the device-walk
  API (`AxlStorageTransport`, `AxlStorageDev`, `axl_storage_next`/`_get_transport`/
  `_get_location`) into a new `include/axl/axl-storage.h`; `axl-smart.h` keeps the
  health API and `#include`s storage.h, so it stays source-compatible (consumers of
  axl-smart.h transitively get the storage symbols — recompile only). Umbrella +
  sphinx (`smart.rst` gains a second doxygenfile) + AXL-Storage-Design.md updated.
- ✅ **E2:** `axl_sntp_query` -> `axl_net_sntp_query`, `AxlSntpResult` ->
  `AxlNetSntpResult` (header + impl + tests + README + integration comments).
- ✅ **E3a:** `AxlHexDumpLog` -> `axl_hexdump_log` (macro). NOTE: "0 callers" was
  in-tree only — Dell axl-utils `doDriver/cmd_sysid.c` calls it (consumer site).
- ✅ **E3b:** whole `axl-crashrecord.h` prefixed — `CRASH_*` -> `AXL_CRASH_*`,
  `SYM_MAGIC` -> `AXL_SYM_MAGIC`, and the six types (`CrashRecordHeader` etc.) ->
  `Axl`-prefixed. ALL VALUES byte-for-byte preserved (persisted NVRAM binary format +
  protocol contract). In-tree consumer = `drivers/crashhandler/` (writer) — updated
  in lockstep; the driver's own guard `CRASH_HANDLER_H` + internal
  `CRASH_REPORT_MAX_VOLUMES` + tool-local `CRASH_MODE_*` correctly left unprefixed.
  rsod-decode (host reader) is Python parsing the bytes — unaffected by C renames.
- ✅ **E3c:** `axl_rb_tree_empty` -> `axl_rb_tree_is_empty`.
- **Skipped (per decision):** drop-`get_` on `axl_queue_get_length`/
  `axl_ring_buf_get_length` (blast ~16) — deferred to avoid the churn; other
  Category-5 cosmetic naming items remain for a later pass.
- 7972/0 both arches (behavior-preserving; no new tests). Independent whole-tree
  review CONFIRMED source-clean (values preserved, locals untouched, E1 source-compat,
  no include cycle); caught 2 doc-prose staleness misses (RBTree + Storage design
  docs) — fixed.

**Batch F — enum-flag + type-hygiene sweep (LOW, ~2-3 hours). ✅ DONE.**
Wrap bare `#define` kind/flag groups in typedef'd enums (`AxlFontFlags`,
`AxlResetType`, `AxlWsEvent`, `AxlCpuArch`), migrate `axl_smbios_*` raw
`char`/`short` params to stdint, const-qualify `axl_string_len` and the ring-buf
read-only accessors. All low-risk; batch as one "tidy" commit.

- ✅ **Enum-wraps (values byte-preserved):** `AxlFontFlags` (font), `AxlResetType`
  (sys; `axl_reset` param `int`->`AxlResetType`), `AxlCpuArch` (cpu; struct field +
  ADDED `AXL_CPU_ARCH_UNKNOWN=0` for the unsupported-arch fallback that was a bare
  `0`), `AxlWsEvent` (http-server), `AxlConfigType` (config; `AxlConfigDesc.type`),
  `AxlShmFlags` (shm; the critic's 2nd-pass gap). Param/field widths unchanged where
  they were `uint32_t` bitmasks.
- ✅ **`AxlWsEvent` callback typing (the one consumer break, Mike approved):**
  `AxlWsHandler` + `AxlWsConnHandler` `size_t event`->`AxlWsEvent event` (+ internal
  dispatch + 9 in-tree test handlers). Breaks WS handler signatures downstream ->
  final task.
- ✅ **smbios stdint:** `unsigned char`->`uint8_t`, `unsigned short`->`uint16_t` on
  get_string/_utf8/find/find_next/version (same underlying types = no-op ABI). Test +
  README updated; dropped now-redundant `(uint16_t*)` self-casts.
- ✅ **const:** `axl_string_len`; ring-buf `is_empty`/`is_full`/`get_length`/
  `get_capacity`/`peek`/`peek_elem`/`peek_nth_elem`/`peek_msg_size` (+ pure static
  helpers `ring_readable`/`ring_copy_out`). LEFT non-const (correct const semantics):
  `peek_regions` (publishes a mutable view into the buffer), `peek_msg` (shares the
  consume-capable `ring_buf_msg_read_internal`).
- **DEFERRED (Mike's call):** `AxlShellSource` value qualification
  (`AXL_SHELL_NONE`->`AXL_SHELL_SOURCE_*`) — pure-cosmetic rename that breaks a
  consumer (agt axterm) for no functional gain; not in the F paragraph. Won't-fix
  unless revisited.
- 7972/0 both arches (behavior-preserving, no new tests). Independent whole-tree
  review confirmed all enum values byte-preserved + no missed implementers + const
  correctness; caught 2 LOW (get_capacity missed const; Config-Design.md stale
  #define block) — both fixed.

*Deliberate/documented deviations to leave alone (record as "won't-fix" so the audit
doesn't re-flag them): `axl_socket_free`/`axl_tcp_close` mode params, `axl_image_unload`
int return, `axl_clock_gettime` POSIX echo, `axl_udp_open` net sub-convention,
`AxlInputCallback`/`AxlNTreeTraverseFunc` GLib-parity bool polarity,
`axl_args_get_uint_offset`, the AxlSubcommand-family (already deprecated).*

*Batch F+ carve-outs (2026-07-20, from the 2nd-pass audit): **axl-math.h** prefixes
are deliberate, NOT a module-prefix violation — `axl_vec2_*`/`AxlVec2` and
`axl_transform_*`/`AxlTransform` are TYPE-based prefixes (same convention as
`axl_array_`/`axl_string_`/`axl_hash_table_`); `axl_sin`/`cos`/`sqrt`/`fabs`/`pow`/
`floor`/`ceil`/`fmod`/`exp`/`ln`/`atan2` are libm echoes (same carve-out as
`axl_clock_gettime`); `axl_lerp`/`clamp`/`smoothstep`/`step`/`min`/`max`/`remap` are
GLSL/shader-idiom names; `axl_clz`/`ctz`/`popcount`/`log2i`/`round_up_pow2`/`sat_*` are
compiler-intrinsic echoes. A mass `axl_math_` rename would break every math consumer
for zero real gain — closed by auditing + documenting, not renaming. **gfx int-vs-void
is convention-correct, NOT an inconsistency:** predicates return `bool`
(`axl_gfx_region_is_empty`/`_contains`), fallible ops return `int`
(`axl_gfx_region_union`/`_subtract`/`_intersect`, all of `axl-gfx-draw.h`), and
teardown / in-place mutation return `void` (`axl_gfx_region_free`/`_clear`/`_translate`).
Category 1e's "region ops return void" claim was inaccurate. **axl-port.h RESOLVED**
(not a carve-out): renamed the header + impl to `axl-io-port.h`/`axl-io-port.c` so the
guard (`AXL_IO_PORT_H`), prefix (`axl_io_port_*`), and filename all agree (also
disambiguates from a network port).*

---

## Completeness-critic — known second-pass gaps (RESOLVED 2026-07-20)

_All three 2nd-pass gaps were audited and closed — see the "Batch F+ carve-outs"
paragraph above. Summary: axl-port was a real fix (renamed to axl-io-port.h); axl-math
and gfx were defensible convention, closed by documenting the carve-outs, not renaming._

**Missed categories:**
- ✅ **Header-granularity naming mismatch RESOLVED:** axl-port.h (guard AXL_IO_PORT_H,
  prefix axl_io_port_*) renamed to **axl-io-port.h** (+ axl-io-port.c) so guard, prefix,
  and filename all agree.
- ✅ **axl-math.h whole-header prefix DOCUMENTED (not a violation):** the bare `axl_`
  names are deliberate — `axl_vec2_*`/`axl_transform_*` are type-prefixes, `axl_sin`/
  `sqrt`/etc. are libm echoes, `lerp`/`clamp`/`smoothstep` are shader idioms, `clz`/
  `popcount` are intrinsic echoes. Recorded as carve-outs; no mass rename (would break
  every math consumer for zero gain).

**Under-covered areas:**
- Category 2 ownership/autoptr — RESOLVED in Batch B expanded pass (all 7 extra handle
  types registered; see below).
- ✅ **axl-math.h AUDITED** — 81 symbols reviewed; all prefixes defensible (carve-outs
  documented above).
- ✅ **gfx int-vs-void RESOLVED (was a non-issue):** the split is convention-correct —
  `bool` predicates, `int` fallible ops (incl. all of gfx-draw + region union/subtract/
  intersect), `void` teardown/in-place-mutation (region free/clear/translate). Category
  1e's "region ops return void" claim was inaccurate (those ops return int).
- Small utility headers — axl-shm flags done (Batch F); axl-port done (renamed); the
  rest (clipboard/watchdog/diag/cache/radix-tree) reviewed, no further real items.

**Corrections / drop as false-positive:**
- Category 2 (autoptr gap) understates scope: it lists 4 but at least 6 more opaque _new/_free (or _open/_close) types register no AXL_DEFINE_AUTOPTR_CLEANUP — AxlGfxGradient (axl-gfx-gradient.h), AxlGfxDisplayList (axl-gfx-display-list.h), AxlCursor (axl-cursor.h), AxlConsoleTerm (axl-console-term.h), AxlFwImage (axl-fw.h), AxlTarWriter+AxlTarReader (axl-tar.h). Same headline C++ RAII hole, unflagged. **RESOLVED — all 7 registered in the Batch B expanded pass (see Category 2 header).**
- Category 9 (enum-flag) misses axl-shm.h AXL_SHM_CREATE/AXL_SHM_EXCL — bare #define bitmask passed as a uint32_t flags param, identical shape to the flagged AXL_FONT_* entry.
- axl_queue_deinit recommendation nuance: axl_queue_clear already exists ('Remove all elements. Queue itself is not freed.') and is the working embedded-teardown path; the fix is to point axl_queue_init users at _clear (or alias _deinit), not that no deinit exists. Bug itself (axl_queue_free calls axl_free on the struct) is confirmed real.

---

# Consumer impact + planned final task (2026-07-20)

> ✅ **CONSUMER UPDATE DONE (2026-07-20)** — all consumers updated on a branch
> `api/axl-sdk-consistency-2026-07-20` (NOT pushed), each built clean against the
> updated SDK (`AXL_SDK_SRC=../axl-sdk make`) except Dell (laptop build):
> - **agt** — surface_new/_free, vterm_new T*, console_device_install reorder,
>   io/mem_phys_read_range reorder, sntp rename, config terminator `{nullptr}`
>   (C++ enum-field), + a pre-existing `axl_tcp_close` mode-param lag. Builds clean.
> - **softbmc** — sntp rename + WS callbacks `size_t event`->`AxlWsEvent`
>   (console_screen_new / console_mirror_install were already migrated). Builds clean.
> - **axl-utils (Dell)** — mem_phys_read_range reorder x2, AxlHexDumpLog->axl_hexdump_log,
>   axl-port.h->axl-io-port.h. Committed on a delldiags branch (build is on the laptop).
> - **axl-webfs** — fully transparent; clean recompile, zero edits.
> - uefi-ssh / videoterm / uefi-devkit / e2e-tests — no affected symbols.
> LESSON: building each consumer against the SDK surfaced two misses a grep alone
> couldn't — a C++-only `int`->`AxlConfigType` aggregate-init break (C consumers are
> unaffected) and a pre-existing `axl_tcp_close` lag. Build, don't just grep.

The applied batches (B/C/D/A1/A2a) change public API that Mike's own downstream
repos consume. Audited **softbmc, agt, axl-utils (Dell), axl-webfs, uefi-ssh,
videoterm, uefi-devkit, e2e-tests**. **FINAL TASK — after all audit batches land,
update the consumers below (branch each repo, don't push):**

**Compile-error sites (loud; renames + arg reorders):**
- **agt**: `axl_surface_create`→`_new` (agt-window.cpp ×2, agt-popup-surface.cpp,
  test/unit/agt-test-surface-host.cpp:121); `axl_surface_destroy`→`_free`
  (agt-popup-surface.cpp:112, agt-window.cpp:257); `axl_vterm_new(&vt,…)!=AXL_OK`→`T*`
  (agt-terminal.cpp:409); `axl_console_device_install(&dev,ops,user,&cfg)`→`(ops,user,&cfg,&dev)`
  (axcon.cpp:143); `axl_mem_phys_read_range(a,n,buf,w)`→`(a,n,w,buf)`
  (memory-hex-source.cpp:37); `axl_io_read_range(a,n,buf,w)`→`(a,n,w,buf)`
  (io-hex-source.cpp:33).
- **softbmc**: `axl_console_screen_new(&scr,…)!=AXL_OK`→`T*` (sol.c:365);
  `axl_console_mirror_install(&mirror,&cfg)`→`(&cfg,&mirror)` (console-mirror.c:216).
- **axl-utils** (`~/work/dell/delldiags/source/src/axl-utils`):
  `axl_mem_phys_read_range(a,span,buf,w)`→`(a,span,w,buf)` (doDriver/cmd_mem.c:77, 335).

**Batch E additions (2026-07-20) — more compile-error sites (loud renames):**
- **softbmc**: `axl_sntp_query`→`axl_net_sntp_query` + `AxlSntpResult`→`AxlNetSntpResult`
  (src/time.c, src/time.h). *(storage.c uses `axl_storage_next`/`axl_smart_health` —
  TRANSPARENT: E1 split is source-compatible via axl-smart.h including axl-storage.h.)*
- **agt**: `AxlSntpResult`→`AxlNetSntpResult` (+`axl_sntp_query`→`axl_net_sntp_query`
  if called) in tools/netcfg.cpp.
- **axl-utils** (Dell): `AxlHexDumpLog`→`axl_hexdump_log` (doDriver/cmd_sysid.c) — the
  audit's "0 callers" for this macro was in-tree-only; this external consumer calls it.
- E3b crashrecord + E3c rb_tree renames: NO consumer hits (rsod-decode is a Python
  byte-parser, doesn't include the C header; no consumer uses AxlRBTree's empty pred).

**Batch F additions (2026-07-20) — one compile-error site (loud):**
- **softbmc** `src/module-manager.c`: the WS callbacks `ws_bridge`/`ws_conn_bridge`
  `size_t event`->`AxlWsEvent event` (their local helper `ws_opcode_of(size_t event)`
  can stay `size_t` — enum converts implicitly, or update for consistency).
- Everything else in F is TRANSPARENT for consumers: enum-wraps keep the same value
  NAMES (softbmc power.c/upgrade.c `AXL_RESET_*`, agt/axl-webfs `AXL_CFG_*` still
  compile as enum constants), the `axl_reset` param `int`->`AxlResetType` and const/
  stdint changes are source-compatible, and no consumer stored the smbios get_string
  return as `unsigned short`.

**A(2a) additions (2026-07-20) — TRANSPARENT for consumers, but audit `== AXL_ERR`:**
- The int->AxlStatus / typed-enum promotions (net_ensure_drivers, tls handshake/read,
  http get/post/put/delete/request, ramdisk create/register_image, spd read/dump_raw/
  decode) are source-compatible: `!= AXL_OK` / `== AXL_OK` / assign-to-int all still
  compile and behave. Consumer sites seen: axl-webfs webfs-protocol-json.c +
  webfs-cache.c (http, `int rc`/`return`), softbmc softbmc.c + virtualmedia.c
  (ramdisk, `!= AXL_OK`), agt netcfg.cpp (net_ensure_drivers, `int rc` vs
  `AXL_NET_DRIVERS_*`). **Only latent risk:** a consumer doing `== AXL_ERR` would now
  MISS a specific code (e.g. a timeout is AXL_TIMEOUT=-3, not AXL_ERR=-1) — none seen
  in the scan, but confirm during the consumer-update pass. No loud compile breaks.

**Transparent (just rebuild against new SDK, no code edits):**
- softbmc `src/storage.c` (`axl_storage_next`/`axl_smart_health`) — E1 header split is
  source-compatible (axl-smart.h includes axl-storage.h); recompile only.
- axl-utils shared-driver — uses the `AXL_SHARED_DRIVER` macro, so the silent
  `axl_shared_driver_unpublish` void*-swap fix is inside the SDK macro.
- softbmc security calls (`axl_rng_bytes`/`axl_jws_verify`/`axl_aead_open`) already
  `==AXL_OK`-checked → new `nodiscard` marks don't break a -Werror build.
- softbmc `axl_udp_send`, agt `axl_loop_iterate_until` — return discarded / params
  unchanged; the AxlStatus / AXL_TIMEOUT changes are source-compatible.

**No consumer implements** `AxlTaskProc`/`AxlTaskComplete`, `AxlPageFillFunc`, or
`AxlByteReader.read` (agt's page-cache is fill-less `_new_shared`; axl-webfs's
`webfs_read` is an `AxlFsProvider.read`, which was NOT changed).

**A(2b) `AxlFsStatus` renumber — SHOWN SAFE (reassessed):** only axl-webfs consumes
`AxlFsStatus` (68 sites in webfs-file.c), all via named constants, no serialization,
no sign/literal/index use; the SDK's `status_to_efi` is a named-case `switch` (not an
array index); `AxlFsStatus` never crosses a wire/protocol boundary (converted to
`EFI_STATUS` before the SimpleFileSystem thunk; libaxl is static-linked per image).
Renumber = source-compatible recompile, ZERO manual edits anywhere. NOT the
high-risk item earlier feared.
