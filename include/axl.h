/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl.h:
 *
 * AXL — AximCode Library for UEFI.
 *
 * UEFI C library — with first-class C++ support — aimed at Linux
 * systems C and C++ developers (glibc / GLib / systemd / libcurl
 * audience) who don't want to learn EDK2 to ship a UEFI binary. The
 * C-shaped API is fully usable from C++ (the axl-c++ driver, RAII via
 * AXL_AUTOPTR). UTF-8 everywhere, standard C types, snake_case
 * functions, PascalCase types — the API shape carried over directly
 * from GLib (AxlLoop ~= GMainLoop, AxlHashTable ~= GHashTable, etc).
 * Public API never returns or accepts EFI_* types.
 *
 * This is the umbrella header. Include this one file to get the
 * entire AXL public API (analogous to GLib's <glib.h>).
 */

#ifndef AXL_H
#define AXL_H

#include <axl/axl-version.h>
#include <axl/axl-macros.h>
#include <axl/axl-debug.h>
#include <axl/axl-types.h>
#include <axl/axl-mem.h>
#include <axl/axl-format.h>
#include <axl/axl-math.h>
#include <axl/axl-string.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-device-path.h>
#include <axl/axl-file-view.h>
#include <axl/axl-log.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-array.h>
#include <axl/axl-list.h>
#include <axl/axl-slist.h>
#include <axl/axl-queue.h>
#include <axl/axl-json.h>
#include <axl/axl-xml.h>
#include <axl/axl-cache.h>
#include <axl/axl-page-cache.h>
#include <axl/axl-radix-tree.h>
#include <axl/axl-ntree.h>
#include <axl/axl-tree.h>
#include <axl/axl-rb-tree.h>
#include <axl/axl-find.h>
#include <axl/axl-regex.h>
#include <axl/axl-text-buffer.h>
#include <axl/axl-piece-tree.h>
#include <axl/axl-ring-buf.h>
#include <axl/axl-digest.h>
#include <axl/axl-crypto.h>
#include <axl/axl-hmac.h>
#include <axl/axl-scram.h>
#include <axl/axl-jose.h>
#include <axl/axl-bytes.h>
#include <axl/axl-compress.h>
#include <axl/axl-path.h>
#include <axl/axl-hexdump.h>
#include <axl/axl-clipboard.h>
#include <axl/axl-shm.h>
#include <axl/axl-sort.h>
#include <axl/axl-tar.h>
#include <axl/axl-time.h>
#include <axl/axl-env.h>
#include <axl/axl-sys.h>
#include <axl/axl-cpu.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-var.h>
#include <axl/axl-attempt.h>
#include <axl/axl-io-port.h>
#include <axl/axl-boot.h>
#include <axl/axl-acpi.h>
#include <axl/axl-sidecar.h>
#include <axl/axl-pci.h>
#include <axl/axl-usb.h>
#include <axl/axl-block.h>
#include <axl/axl-nvme.h>
#include <axl/axl-ata.h>
#include <axl/axl-scsi.h>
#include <axl/axl-storage.h>
#include <axl/axl-smart.h>
#include <axl/axl-serial.h>
#include <axl/axl-fv.h>
#include <axl/axl-fw.h>
#include <axl/axl-tpm.h>
#include <axl/axl-hii.h>
#include <axl/axl-ramdisk.h>
#include <axl/axl-driver.h>
#include <axl/axl-driver-deps.h>
#include <axl/axl-driver-info.h>
#include <axl/axl-embed.h>
#include <axl/axl-service.h>
#include <axl/axl-shared-driver.h>
#include <axl/axl-efi-status.h>
#include <axl/axl-image.h>
#include <axl/axl-shell.h>
#include <axl/axl-console-ops.h>
#include <axl/axl-console-tap.h>
#include <axl/axl-console-device.h>
#include <axl/axl-console-mirror.h>
#include <axl/axl-console-vt-enc.h>
#include <axl/axl-console-tee.h>
#include <axl/axl-console-term.h>
#include <axl/axl-console-screen.h>
#include <axl/axl-mem-phys.h>
#include <axl/axl-mem-region.h>
#include <axl/axl-watchdog.h>
#include <axl/axl-rng.h>
#include <axl/axl-rand.h>
#include <axl/axl-diag.h>
#include <axl/axl-config.h>
#include <axl/axl-config-file.h>
#include <axl/axl-subcommand.h>
#include <axl/axl-args.h>
#include <axl/axl-console.h>
#include <axl/axl-image-verify.h>
#include <axl/axl-event.h>
#include <axl/axl-loop.h>
#include <axl/axl-defer.h>
#include <axl/axl-pubsub.h>
#include <axl/axl-wait.h>
#include <axl/axl-cancellable.h>
#include <axl/axl-runtime.h>
#include <axl/axl-atexit.h>
#include <axl/axl-signal.h>
#include <axl/axl-app.h>
#include <axl/axl-task.h>
#include <axl/axl-buf-pool.h>
#include <axl/axl-async.h>
#include <axl/axl-net.h>
#include <axl/axl-net-opts.h>
#include <axl/axl-9p.h>
#include <axl/axl-gfx.h>
#include <axl/axl-edid.h>
#include <axl/axl-input.h>
#include <axl/axl-cursor.h>
#include <axl/axl-smbios.h>
#include <axl/axl-smbus.h>
#include <axl/axl-ipmi.h>
#include <axl/axl-spd.h>

// ---------------------------------------------------------------------------
// AXL_TOOL_MAIN — tool entry point with optional busybox-style dispatch
// ---------------------------------------------------------------------------

/**
 * AXL_TOOL_MAIN(name):
 *
 * Per-tool entry-point declaration. Replaces the bare `int main(...)`
 * line in each tool's source. In the default per-tool build it expands
 * to `int main(int argc, char **argv)` — a tool compiled this way
 * produces the usual standalone `<tool>.efi` binary.
 *
 * In the busybox build (compile with `-DAXL_BUSYBOX`), the same
 * macro instead expands to `int axl_tool_<name>_main(int argc,
 * char **argv)` — every tool's body becomes a regular function in a
 * single fat binary, dispatched by `tools/axl.c`'s verb tree.
 *
 * Use at file scope, immediately around the tool's body:
 *
 *     AXL_TOOL_MAIN(cat) {
 *         AxlArgs *a = ... ;
 *         return axl_args_run(...);
 *     }
 *
 * @c name should match the .c filename's stem so the busybox
 * dispatcher's `extern` declarations resolve. The tools/axl.c
 * dispatcher table is the source of truth for which names exist.
 */
#ifdef AXL_BUSYBOX
  #define AXL_TOOL_ENTRY_(name) int axl_tool_##name##_main(int argc, char **argv)
#else
  #define AXL_TOOL_ENTRY_(name) int main(int argc, char **argv)
#endif

/* Wrap the tool body with a uniform `--version` / `-V` pre-check so EVERY
 * tool reports the SDK release version identically, whether or not it uses
 * the axl_args_run parser. The tool's `{ ... }` block becomes the static body;
 * the entry point answers a version request (printing "<name> <version>" and
 * returning 0) before the body ever runs. `#name` is the tool stem passed to
 * the macro (e.g. "mkrd"). */
#define AXL_TOOL_MAIN(name)                                            \
    static int _axl_tool_body_##name(int argc, char **argv);          \
    AXL_TOOL_ENTRY_(name) {                                           \
        if (axl_version_handle(#name, argc, argv)) {                   \
            return 0;                                                  \
        }                                                              \
        return _axl_tool_body_##name(argc, argv);                     \
    }                                                                  \
    static int _axl_tool_body_##name(int argc, char **argv)

// ---------------------------------------------------------------------------
// AXL_APP — application entry point
// ---------------------------------------------------------------------------

void _axl_init(void *image_handle, void *system_table);
void _axl_get_args(int *argc, char ***argv);
void _axl_cleanup(void);

/**
 * AXL_APP:
 * @main_func: your int main(int argc, char **argv) function
 *
 * Defines the UEFI entry point. Use at file scope:
 *
 *   int my_main(int argc, char **argv) { ... }
 *   AXL_APP(my_main)
 *
 * Not needed for SDK builds — the SDK provides axl-crt0-native.c
 * which bridges int main() to the UEFI entry point automatically.
 */
/* The AXL_APP / AXL_DRIVER entry-point macros below emit firmware-ABI
 * entry symbols, but express them entirely in AXL-native types
 * (AxlEfiStatus, AxlHandle, AxlSystemTable *, AXLAPI) — all uefi-free —
 * so the <axl.h> umbrella does NOT pull <uefi/...> and never leaks
 * EFI_* into consumers. AxlEfiStatus is binary-compatible with
 * EFI_STATUS and AxlHandle / AxlSystemTable * are pointer-ABI-identical
 * to EFI_HANDLE / EFI_SYSTEM_TABLE *, so the emitted DriverEntry /
 * _AxlEntry are byte-for-byte the firmware entry points. A consumer that
 * genuinely needs spec UEFI types (publishing EFI_FILE_PROTOCOL, calling
 * gBS->... directly) opts into <uefi/axl-uefi.h> explicitly. */

/**
 * AXLAPI:
 *
 * Calling convention macro for callbacks the firmware invokes
 * directly (custom protocol entry points, raw event notifies,
 * shell command dispatchers). On the UEFI backend this is
 * `EFIAPI` (Microsoft-x64 / AAPCS64 with the spec's per-arch
 * conventions). Drivers using `AXL_DRIVER` don't need to spell
 * `AXLAPI` themselves — the macro already adapts; this is for
 * the genuine escape-hatch case where a consumer registers a
 * firmware-called function outside the AXL helper surface.
 */
#define AXLAPI AXL_EFI_ABI

/**
 * AXL_ENTRY_LINKAGE:
 *
 * Force C linkage on the firmware entry-point symbol a macro emits
 * (`_AxlEntry` / `DriverEntry`). In a C++ translation unit the entry
 * function would otherwise be name-mangled, so the linker — which
 * resolves the image entry by its exact unmangled name (drivers link
 * with `--defsym=_AxlEntry=DriverEntry`) — fails with an undefined
 * `DriverEntry`. Expands to `extern "C"` under C++ and to nothing in
 * C, so a C++ driver/app needs no hand-written `extern "C"` wrapper.
 * It applies only to the emitted entry symbol; the consumer's own
 * entry/unload functions keep normal (C++) linkage.
 */
#ifdef __cplusplus
#define AXL_ENTRY_LINKAGE extern "C"
#else
#define AXL_ENTRY_LINKAGE
#endif

#define AXL_APP(main_func)                                            \
  int main_func(int, char **);                                        \
  AXL_ENTRY_LINKAGE AxlEfiStatus                                      \
  AXLAPI                                                              \
  _AxlEntry(AxlHandle ImageHandle, AxlSystemTable *SystemTable) {     \
    _axl_init((void *)ImageHandle, (void *)SystemTable);             \
    int _axl_argc; char **_axl_argv;                                  \
    _axl_get_args(&_axl_argc, &_axl_argv);                            \
    int _axl_rc = main_func(_axl_argc, _axl_argv);                   \
    _axl_cleanup();                                                    \
    return (_axl_rc == 0) ? AXL_EFI_SUCCESS : AXL_EFI_ABORTED;        \
  }

/**
 * AXL_DRIVER:
 * @entry_func: int entry(AxlHandle image, AxlSystemTable *st)
 * @unload_func: int unload(AxlHandle image)
 *
 * Defines the UEFI DriverEntry + Unload entry points for a DXE
 * driver image without making the consumer write `EFIAPI`,
 * `EFI_STATUS`, `EFI_HANDLE`, or `EFI_SYSTEM_TABLE` themselves.
 * Wires `axl_driver_init` and `axl_driver_set_unload`
 * automatically. Consumer entry/unload return `int` — 0 means
 * success (the macro maps to `EFI_SUCCESS`), non-zero aborts the
 * load (mapped to `EFI_ABORTED`).
 *
 * @code
 *   static int my_main(AxlHandle image, AxlSystemTable *st);
 *   static int my_unload(AxlHandle image);
 *
 *   AXL_DRIVER(my_main, my_unload)
 *
 *   static int my_main(AxlHandle image, AxlSystemTable *st) {
 *       (void)image; (void)st;
 *       // ... axl_protocol_register, axl_loop_attach_driver, ...
 *       return 0;
 *   }
 *
 *   static int my_unload(AxlHandle image) {
 *       (void)image;
 *       // ... axl_protocol_unregister, axl_loop_detach_driver, ...
 *       return 0;
 *   }
 * @endcode
 *
 * Consumers needing UEFI types beyond what AXL exposes (publishing
 * a spec-defined protocol struct like `EFI_FILE_PROTOCOL`, calling
 * `gBS->...` directly) opt into `<uefi/axl-uefi.h>` separately —
 * the macro doesn't preclude that.
 *
 * Type-match caveat: `AxlHandle` is itself a `void *` typedef and
 * `AxlSystemTable *` is implicitly convertible from `void *`, so
 * the compiler will silently accept consumer functions declared
 * with mismatched parameter types (`int my_main(void *, void *)`
 * compiles without a warning against the macro's forward decl).
 * Use the documented signatures literally so future readers see
 * the intent.
 */
#define AXL_DRIVER(entry_func, unload_func)                              \
  int entry_func(AxlHandle, AxlSystemTable *);                           \
  int unload_func(AxlHandle);                                            \
                                                                         \
  static AxlEfiStatus AXLAPI                                             \
  _axl_driver_unload_stub(AxlHandle _img) {                              \
    int _rc = unload_func(_img);                                         \
    return (_rc == 0) ? AXL_EFI_SUCCESS : AXL_EFI_ABORTED;               \
  }                                                                      \
                                                                         \
  AXL_ENTRY_LINKAGE AxlEfiStatus AXLAPI                                                    \
  DriverEntry(AxlHandle _ImageHandle, AxlSystemTable *_SystemTable) {    \
    axl_driver_init(_ImageHandle, _SystemTable);                         \
    axl_driver_set_unload((void *)_axl_driver_unload_stub);              \
    int _rc = entry_func(_ImageHandle, _SystemTable);                    \
    return (_rc == 0) ? AXL_EFI_SUCCESS : AXL_EFI_ABORTED;               \
  }

/**
 * AXL_SHARED_DRIVER(name_str, init_fn, run_fn, unload_fn):
 *   int init_fn(void)           — heavy per-boot setup; 0 = ok (else abort load)
 *   int run_fn(int, char **)    — per-dispatch entry (== int main)
 *   int unload_fn(void)         — teardown; 0 = ok
 *
 * Emits the driver image's DriverEntry/Unload: runs init_fn once, publishes
 * the SDK-standard AxlSharedDriverVtable{.run=run_fn} under @p name_str; the
 * unload path unpublishes then calls unload_fn. The consumer writes only the
 * three functions — no vtable, no publish/unpublish, no AXL_DRIVER.
 *
 * Like `AXL_DRIVER`, this macro forward-declares @p init_fn / @p run_fn /
 * @p unload_fn with external linkage. Define the three functions `static`
 * FIRST, then invoke the macro LAST — a later non-static forward decl of
 * an already-`static` name retains internal linkage (no error), but the
 * reverse order (`static` definition after this macro's non-static forward
 * decl) fails with "static declaration follows non-static declaration".
 *
 * @code
 *   static int my_init(void);
 *   static int my_run(int argc, char **argv);
 *   static int my_unload(void);
 *
 *   static int my_init(void) {
 *       // ... one-time per-boot setup ...
 *       return 0;
 *   }
 *
 *   static int my_run(int argc, char **argv) {
 *       // ... per-dispatch entry, like int main ...
 *       return 0;
 *   }
 *
 *   static int my_unload(void) {
 *       // ... teardown ...
 *       return 0;
 *   }
 *
 *   AXL_SHARED_DRIVER("my-driver", my_init, my_run, my_unload)
 * @endcode
 *
 * Use AXL_SHARED_DRIVER xor AXL_DRIVER xor AXL_SERVICE_DRIVER per
 * translation unit — each emits the image's single DriverEntry.
 */
#define AXL_SHARED_DRIVER(name_str, init_fn, run_fn, unload_fn)             \
  int init_fn(void);                                                        \
  int run_fn(int, char **);                                                 \
  int unload_fn(void);                                                      \
  static AxlSharedDriverVtable _axl_sd_vtable = { run_fn };                 \
  static AxlHandle             _axl_sd_handle = NULL;                       \
  static int _axl_sd_entry(AxlHandle _h, AxlSystemTable *_st) {             \
    (void)_h; (void)_st;                                                    \
    int _rc = init_fn();                                                    \
    if (_rc != 0) { return _rc; }                                           \
    return axl_shared_driver_publish((name_str), &_axl_sd_vtable,           \
                                      &_axl_sd_handle);                     \
  }                                                                         \
  static int _axl_sd_unload(AxlHandle _h) {                                 \
    (void)_h;                                                               \
    if (_axl_sd_handle != NULL) {                                           \
      axl_shared_driver_unpublish((name_str), &_axl_sd_vtable,              \
                                   _axl_sd_handle);                         \
    }                                                                       \
    return unload_fn();                                                     \
  }                                                                         \
  AXL_DRIVER(_axl_sd_entry, _axl_sd_unload)

/**
 * AXL_SHARED_DRIVER_LAUNCHER(name_str, driver_filename, embed_symbol):
 * The entire launcher `int main` — resolves the resident driver (resident →
 * on-disk → embedded @p embed_symbol) and dispatches with stdio + exit-status
 * bridged. @p embed_symbol is an AXL_EMBED name (the driver's .efi bytes
 * linked in via the build's embed step).
 */
#define AXL_SHARED_DRIVER_LAUNCHER(name_str, driver_filename, embed_symbol) \
  AXL_EMBED_DECLARE(embed_symbol);                                          \
  int main(int argc, char **argv) {                                         \
    return axl_shared_driver_run((name_str), (driver_filename),             \
                                  AXL_EMBED_DATA(embed_symbol),             \
                                  AXL_EMBED_SIZE(embed_symbol),             \
                                  argc, argv);                              \
  }

/**
 * AXL_SHARED_DRIVER_LAUNCHER_THIN(name_str, driver_filename):
 * Like AXL_SHARED_DRIVER_LAUNCHER but NO embedded blob — loads the driver
 * from disk only (resident → on-disk). Smallest per-command transfer.
 */
#define AXL_SHARED_DRIVER_LAUNCHER_THIN(name_str, driver_filename)          \
  int main(int argc, char **argv) {                                         \
    return axl_shared_driver_run((name_str), (driver_filename),             \
                                  (const unsigned char *)NULL, 0, argc,     \
                                  argv);                                    \
  }

/**
 * AXL_SHARED_DRIVER_LAUNCHER_SIBLING(name_str, driver_filename):
 * Like AXL_SHARED_DRIVER_LAUNCHER_THIN but SIBLING-ONLY (version-pinned):
 * hard-fails (no /drivers, no volume-root, no cross-volume search) if
 * @p driver_filename isn't staged beside this launcher image. For launchers
 * that must pair with the exact driver co-staged with them.
 */
#define AXL_SHARED_DRIVER_LAUNCHER_SIBLING(name_str, driver_filename)       \
  int main(int argc, char **argv) {                                         \
    return axl_shared_driver_run_sibling((name_str), (driver_filename),    \
                                         argc, argv);                       \
  }

/**
 * AXL_SERVICE_DRIVER:
 * @svc: an `AxlService` lvalue (typically a `static const`)
 *
 * Driver-image counterpart to `axl_service_start_embedded` /
 * `AxlServiceDeploy` on the foreground side. Emits the firmware
 * `DriverEntry` symbol that delegates to the SDK library
 * (@ref _axl_service_driver_init): backend init, LoadOptions
 * decode, protocol publish, loop creation, and attach against
 * `svc.driver_tick_ms` (defaults to 50 ms when zero).
 *
 * The driver image must link the same `svc` descriptor object the
 * foreground app describes. The cross-binary ABI tripwire applies
 * (see `<axl/axl-service.h>`): both binaries must be built from
 * the same source tree with identical compile flags.
 *
 * @code
 *   // shared between foreground and driver image:
 *   typedef struct { uint16_t port; bool verbose; } MyOpts;
 *   static MyOpts opts;
 *   static const AxlConfigDesc opts_descs[] = { ... { 0 } };
 *   static int my_setup(AxlLoop *loop, void *user) { ... }
 *   static int my_teardown(void *user) { ... }
 *   static const AxlService my_service = {
 *       .name           = "my-service",
 *       .opts_descs     = opts_descs,
 *       .setup          = my_setup,
 *       .teardown       = my_teardown,
 *       .user           = &opts,
 *       .driver_tick_ms = 50,
 *   };
 *
 *   // in the driver image's only .c file:
 *   AXL_SERVICE_DRIVER(my_service);
 * @endcode
 *
 * On LoadOptions decode failure the library logs and aborts the
 * load (firmware sees EFI_ABORTED → driver image is unloaded).
 * Setup failure follows the same path.
 *
 * **Use AXL_SERVICE_DRIVER xor AXL_DRIVER per translation unit.**
 * Both expand to a `DriverEntry` symbol; defining both produces a
 * link-time multiple-definition error. AXL_SERVICE_DRIVER is the
 * higher-level wrapper for the loop-and-options pattern; AXL_DRIVER
 * is the bare-bones macro for drivers that don't run a service
 * (publish-protocol-and-leave style). Pick one.
 */
/* No cast on the returned status: _axl_service_driver_init already
 * returns AxlEfiStatus. A widening cast here used to hide the fact
 * that the callee narrowed to `int` first, stripping the EFI error
 * bit and turning every setup failure into a "successful" load. */
#define AXL_SERVICE_DRIVER(svc)                                            \
  AXL_ENTRY_LINKAGE AxlEfiStatus AXLAPI                                                       \
  DriverEntry(AxlHandle _ImageHandle, AxlSystemTable *_SystemTable) {       \
    return _axl_service_driver_init(                                        \
               (void *)_ImageHandle, (void *)_SystemTable, &(svc));         \
  }

/**
 * AXL_SERVICE:
 * @svc: an `AxlService` lvalue (typically a `static const`)
 *
 * One-shot single-source-file service: emits whichever entry point
 * the current build needs. Pair with `axl-cc --service NAME source.c`
 * (which compiles the same source twice — once with
 * `-DAXL_SERVICE_BUILD_DRIVER` for the driver image, once without
 * for the launcher app — and embeds the driver into the launcher).
 *
 * - When `AXL_SERVICE_BUILD_DRIVER` is defined (driver-image
 *   compile), expands to @ref AXL_SERVICE_DRIVER.
 * - Otherwise (launcher-app compile), expands to a `main()` that
 *   declares the embedded driver blob (matching the symbol name
 *   `axl-cc --service` emits) and delegates to
 *   @ref axl_service_main with a deploy descriptor pre-filled.
 *
 * @code
 *   // service.c — single source file
 *   #include <axl.h>
 *
 *   typedef struct { uint16_t port; bool verbose; } MyOpts;
 *   static MyOpts opts;
 *   static const AxlConfigDesc opts_descs[] = { ... { 0 } };
 *   static int my_setup(AxlLoop *loop, void *user) { ... }
 *   static int my_teardown(void *user) { ... }
 *   static const AxlService my_service = {
 *       .name           = "my-service",
 *       .opts_descs     = opts_descs,
 *       .setup          = my_setup,
 *       .teardown       = my_teardown,
 *       .user           = &opts,
 *       .driver_tick_ms = 50,
 *   };
 *
 *   AXL_SERVICE(my_service);   // emits DriverEntry or main()
 * @endcode
 *
 * Build:
 * @code
 *   axl-cc --service my-service service.c
 *   # produces: my-service.efi (launcher) + my-service-dxe.efi (driver)
 * @endcode
 *
 * For consumers that need a custom verb tree (multi-service tools,
 * extra subcommands), don't use this macro — write your own main()
 * that calls `axl_service_main(&deploy, argc, argv)` directly, or
 * mix the standard verbs with your own via `axl_args_run`.
 */
#ifdef AXL_SERVICE_BUILD_DRIVER
  #define AXL_SERVICE(svc) AXL_SERVICE_DRIVER(svc)
#else
  #define AXL_SERVICE(svc)                                                  \
    AXL_EMBED_DECLARE(svc);                                                 \
    int main(int argc, char **argv) {                                       \
      AxlServiceDeploy _axl_svc_deploy = {                                  \
        .service          = &(svc),                                         \
        .driver_blob      = AXL_EMBED_DATA(svc),                            \
        .driver_blob_len  = AXL_EMBED_SIZE(svc),                            \
        .driver_name      = #svc "-dxe.efi",                                \
      };                                                                    \
      return axl_service_main(&_axl_svc_deploy, argc, argv);                \
    }
#endif

#endif /* AXL_H */
