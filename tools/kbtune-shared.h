/* kbtune-shared.h — config channel shared by the kbtune launcher and kbtune-drv.
 *
 * The resident driver (kbtune-drv) publishes a KbTuneVtable under the shared-driver
 * identity "kbtune" (axl_shared_driver_guid("kbtune")); the launcher (kbtune)
 * locates it and calls get/set to read back or commit the conditioning config.
 *
 * Only the SOURCE-SIDE conditioners persist here — a resident ConIn shim can drop
 * a too-fast same-key repeat (debounce) and space out delivery (min-gap), but it
 * cannot change the shell's own read/redraw cadence. kbtune's drain/stall/redraw
 * knobs are UI-only simulation and are deliberately NOT part of this struct.
 */

#ifndef KBTUNE_SHARED_H
#define KBTUNE_SHARED_H

#include <stdint.h>
#include <stdbool.h>

/* Shared-driver identity (both halves derive the same GUID from this). */
#define KBTUNE_SHARED_NAME     "kbtune"

/* Bump when the wire layout of AxlKbTuneConfig / KbTuneVtable changes. */
#define KBTUNE_CONFIG_VERSION  1u
#define KBTUNE_VTABLE_VERSION  1u

/* The persisted conditioning config. */
typedef struct {
    uint32_t version;         /* KBTUNE_CONFIG_VERSION */
    bool     enabled;         /* false: the wrap relays every key unconditioned */
    uint32_t debounce_ms;     /* drop a same-key repeat faster than this (0 = off) */
    uint32_t min_gap_ms;      /* min spacing between delivered keys (0 = off) */
    bool     printable_only;  /* exempt navigation/editing keys from both */
} AxlKbTuneConfig;

/* The vtable kbtune-drv publishes (consumer-owned; not the SDK {run} vtable).
 * The launcher casts the located interface to this. Both functions return
 * AXL_OK / AXL_ERR. */
typedef struct {
    uint32_t version;                          /* KBTUNE_VTABLE_VERSION */
    int (*get)(AxlKbTuneConfig *out);          /* copy the live config out */
    int (*set)(const AxlKbTuneConfig *in);     /* apply a config live to the wrap */
} KbTuneVtable;

#endif /* KBTUNE_SHARED_H */
