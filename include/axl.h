/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl.h:
 *
 * AXL — AximCode Library for UEFI.
 *
 * UEFI C library aimed at Linux systems C developers (glibc / GLib /
 * systemd / libcurl audience) who don't want to learn EDK2 to ship
 * a UEFI binary. UTF-8 everywhere, standard C types, snake_case
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
#include <axl/axl-types.h>
#include <axl/axl-mem.h>
#include <axl/axl-format.h>
#include <axl/axl-string.h>
#include <axl/axl-str.h>
#include <axl/axl-io.h>
#include <axl/axl-log.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-array.h>
#include <axl/axl-list.h>
#include <axl/axl-slist.h>
#include <axl/axl-queue.h>
#include <axl/axl-json.h>
#include <axl/axl-cache.h>
#include <axl/axl-radix-tree.h>
#include <axl/axl-ring-buf.h>
#include <axl/axl-digest.h>
#include <axl/axl-path.h>
#include <axl/axl-hexdump.h>
#include <axl/axl-time.h>
#include <axl/axl-env.h>
#include <axl/axl-sys.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-driver.h>
#include <axl/axl-diag.h>
#include <axl/axl-config.h>
#include <axl/axl-subcommand.h>
#include <axl/axl-event.h>
#include <axl/axl-loop.h>
#include <axl/axl-defer.h>
#include <axl/axl-pubsub.h>
#include <axl/axl-wait.h>
#include <axl/axl-cancellable.h>
#include <axl/axl-runtime.h>
#include <axl/axl-atexit.h>
#include <axl/axl-signal.h>
#include <axl/axl-task.h>
#include <axl/axl-buf-pool.h>
#include <axl/axl-async.h>
#include <axl/axl-net.h>
#include <axl/axl-gfx.h>
#include <axl/axl-smbios.h>
#include <axl/axl-smbus.h>
#include <axl/axl-ipmi.h>

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
/* Include UEFI types for AXL_APP macro */
#include <uefi/axl-uefi.h>

#define AXL_APP(main_func)                                            \
  int main_func(int, char **);                                        \
  EFI_STATUS                                                          \
  EFIAPI                                                              \
  _AxlEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {  \
    _axl_init(ImageHandle, SystemTable);                              \
    int _axl_argc; char **_axl_argv;                                  \
    _axl_get_args(&_axl_argc, &_axl_argv);                            \
    int _axl_rc = main_func(_axl_argc, _axl_argv);                   \
    _axl_cleanup();                                                    \
    return (_axl_rc == 0) ? EFI_SUCCESS : EFI_ABORTED;                \
  }

#endif /* AXL_H */
