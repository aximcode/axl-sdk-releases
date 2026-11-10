/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-timer.c
    Native UEFI backend -- firmware timer resolution requests.

    The PI Timer Architectural Protocol's period is the tick under every
    timer event; axl_timer_resolution_begin/end (axl-time.h) raise it the
    way Windows' timeBeginPeriod does. The tick is firmware-global, so the
    requests are too: every AXL image shares ONE registry, kept in a UEFI
    configuration table (EDK2's own mechanism for state that outlives an
    image), holding the period found before the first request and every
    active request with its owner. Any image's begin or end recomputes the
    finest period over all of them. The layout's version is part of the
    table's GUID, so an image built against another layout simply does not
    see this one (and keeps its own) rather than misreading it.

    An image's requests go with it: an axl_atexit handler ends them on
    every AXL exit path, and a request whose image is gone without that
    (a crash, a raw gBS->Exit) is dropped at the next begin or end. A
    reloaded image can be given the very handle a dead one had, so each
    image also stamps its requests with a token of its own -- an entry with
    THIS image's handle and another token is a dead predecessor's -- and
    records its owner's load address and size: a handle another image now
    holds is alive, but not the same image.
**/

#include "axl-backend.h"
#include <axl/axl-atexit.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-time.h>

AXL_LOG_DOMAIN("backend");

// ---------------------------------------------------------------------------
// Registry layout (shared by every AXL image: change only with a new version)
// ---------------------------------------------------------------------------

#define TIMER_REG_MAGIC    0x52545841u   /* "AXTR" */
#define TIMER_REG_SLOTS    32u
#define TIMER_100NS_PER_US 10u

typedef struct {
    uint32_t id;          /* 0: free */
    uint32_t period_us;
    void    *owner;       /* the requesting image's handle */
    uint64_t token;       /* that image's own stamp; see the file comment */
    uint64_t image_base;  /* the owner's LoadedImage, when it asked */
    uint64_t image_size;
} TimerRequest;

typedef struct {
    uint32_t     magic;
    uint32_t     next_id;
    uint32_t     found_valid;   /* found_100ns holds the pre-request period */
    uint64_t     found_100ns;
    uint64_t     last_set_100ns;
    TimerRequest req[TIMER_REG_SLOTS];
} TimerRegistry;

/* The layout's identity: a change to TimerRequest or TimerRegistry needs a
   NEW GUID, so images of either layout keep to their own table.
   Layout 1: {6a1d3c4e-9b2f-4c7a-8e51-2d930f6ba417} */
static EFI_GUID mTimerRegistryGuid =
    { 0x6a1d3c4e, 0x9b2f, 0x4c7a, { 0x8e, 0x51, 0x2d, 0x93, 0x0f, 0x6b, 0xa4, 0x17 } };

static uint64_t mToken;
static uint32_t mAtexit;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static EFI_TIMER_ARCH_PROTOCOL *
timer_arch(void)
{
    EFI_TIMER_ARCH_PROTOCOL *t = NULL;
    if (EFI_ERROR(gBS->LocateProtocol(&gEfiTimerArchProtocolGuid, NULL, (void **)&t))) {
        return NULL;
    }
    return t;
}

static uint64_t
timer_get(
    EFI_TIMER_ARCH_PROTOCOL *t
    )
{
    UINT64 p = 0;
    if (EFI_ERROR(t->GetTimerPeriod(t, &p))) {
        return 0;
    }
    return p;
}

/* The shared registry, or NULL. @p create installs it on first use. */
static TimerRegistry *
registry(
    bool create
    )
{
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &gST->ConfigurationTable[i];
        if (axl_memcmp(&e->VendorGuid, &mTimerRegistryGuid, sizeof(EFI_GUID)) == 0) {
            TimerRegistry *r = e->VendorTable;
            /* Not ours under our GUID: refuse it rather than misread it.
               No log here -- this runs at TPL_NOTIFY, and the caller's
               failure status already says it. */
            return (r != NULL && r->magic == TIMER_REG_MAGIC) ? r : NULL;
        }
    }
    if (!create) {
        return NULL;
    }
    /* Firmware pool, NOT AXL's heap: it must outlive this image. */
    TimerRegistry *r = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiBootServicesData, sizeof(*r), (void **)&r))) {
        return NULL;
    }
    axl_memset(r, 0, sizeof(*r));
    r->magic   = TIMER_REG_MAGIC;
    r->next_id = 1;
    if (EFI_ERROR(gBS->InstallConfigurationTable(&mTimerRegistryGuid, r))) {
        gBS->FreePool(r);
        return NULL;
    }
    return r;
}

static uint64_t
own_token(void)
{
    if (mToken == 0) {
        mToken = (axl_time_get_us() << 16) ^ (uint64_t)(uintptr_t)&mToken;
        if (mToken == 0) {
            mToken = 1;
        }
    }
    return mToken;
}

/* The image behind @p handle as a request records it; false if none. */
static bool
image_identity(
    void     *handle,
    uint64_t *base,
    uint64_t *size
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)handle, &gEfiLoadedImageProtocolGuid,
                                      (void **)&li)) || li == NULL) {
        return false;
    }
    *base = (uint64_t)(uintptr_t)li->ImageBase;
    *size = li->ImageSize;
    return true;
}

static bool
request_alive(
    const TimerRequest *q
    )
{
    if (q->owner == (void *)gImageHandle) {
        return q->token == own_token();
    }
    uint64_t base = 0;
    uint64_t size = 0;
    return image_identity(q->owner, &base, &size)
           && base == q->image_base && size == q->image_size;
}

static void
prune(
    TimerRegistry *r
    )
{
    for (uint32_t i = 0; i < TIMER_REG_SLOTS; i++) {
        if (r->req[i].id != 0 && !request_alive(&r->req[i])) {
            axl_memset(&r->req[i], 0, sizeof(r->req[i]));
        }
    }
}

/* Put the tick where the active requests say: the finest of them, or --
   with none left -- back to the period found, if the tick is still the one
   AXL set (something outside AXL may have moved it since). False if the
   firmware refused the period. */
static bool
apply(
    TimerRegistry           *r,
    EFI_TIMER_ARCH_PROTOCOL *t
    )
{
    uint32_t finest = 0;
    for (uint32_t i = 0; i < TIMER_REG_SLOTS; i++) {
        if (r->req[i].id != 0 && (finest == 0 || r->req[i].period_us < finest)) {
            finest = r->req[i].period_us;
        }
    }
    if (!r->found_valid) {
        return true;
    }
    uint64_t now = timer_get(t);
    if (finest == 0) {
        if (now == r->last_set_100ns && r->found_100ns != 0 && now != r->found_100ns) {
            t->SetTimerPeriod(t, r->found_100ns);
        }
        r->found_valid = 0;
        return true;
    }
    uint64_t want = (uint64_t)finest * TIMER_100NS_PER_US;
    if (want > r->found_100ns) {
        want = r->found_100ns;   /* never coarser than it was found */
    }
    bool ok = (now == want) || !EFI_ERROR(t->SetTimerPeriod(t, want));
    r->last_set_100ns = timer_get(t);
    return ok;
}

/* End every request this image still holds: on every AXL exit path. */
static void
release_own(
    void *data
    )
{
    (void)data;
    EFI_TIMER_ARCH_PROTOCOL *t = timer_arch();
    TimerRegistry           *r = registry(false);
    if (t == NULL || r == NULL) {
        return;
    }
    EFI_TPL old = gBS->RaiseTPL(TPL_NOTIFY);
    for (uint32_t i = 0; i < TIMER_REG_SLOTS; i++) {
        if (r->req[i].id != 0 && r->req[i].owner == (void *)gImageHandle
            && r->req[i].token == own_token()) {
            axl_memset(&r->req[i], 0, sizeof(r->req[i]));
        }
    }
    prune(r);
    apply(r, t);   /* exit path: nothing to report a refusal to */
    gBS->RestoreTPL(old);
}

// ---------------------------------------------------------------------------
// Backend API
// ---------------------------------------------------------------------------

int
axl_backend_timer_resolution_begin(
    uint32_t  period_us,
    uint32_t *out_id,
    uint32_t *out_granted_us
    )
{
    EFI_TIMER_ARCH_PROTOCOL *t = timer_arch();
    if (t == NULL) {
        return AXL_UNSUPPORTED;
    }
    EFI_TPL old = gBS->RaiseTPL(TPL_NOTIFY);
    int rc = AXL_ERR;
    TimerRegistry *r = registry(true);
    if (r == NULL) {
        goto out;
    }
    /* Apply what pruning left BEFORE reading the tick as "found": a dead
       image's request may still be holding it fast, and the period to
       restore is the one before that request, not its leftover. */
    prune(r);
    apply(r, t);   /* a refusal here concerns other requests, not this one */
    bool any = false;
    for (uint32_t i = 0; i < TIMER_REG_SLOTS; i++) {
        any = any || r->req[i].id != 0;
    }
    if (!any) {
        uint64_t now = timer_get(t);
        if (now == 0) {
            rc = AXL_UNSUPPORTED;   /* a stopped timer: never ours to restart */
            goto out;
        }
        r->found_100ns    = now;
        r->last_set_100ns = now;
        r->found_valid    = 1;
    }
    TimerRequest *slot = NULL;
    for (uint32_t i = 0; i < TIMER_REG_SLOTS && slot == NULL; i++) {
        if (r->req[i].id == 0) {
            slot = &r->req[i];
        }
    }
    if (slot == NULL) {
        goto out;
    }
    if (r->next_id == 0) {
        r->next_id = 1;
    }
    slot->id        = r->next_id++;
    slot->period_us = period_us;
    slot->owner     = (void *)gImageHandle;
    slot->token     = own_token();
    if (!image_identity(gImageHandle, &slot->image_base, &slot->image_size)
        || !apply(r, t)) {
        /* The firmware refused the period: this request never took effect. */
        axl_memset(slot, 0, sizeof(*slot));
        apply(r, t);
        goto out;
    }
    *out_id = slot->id;
    if (out_granted_us != NULL) {
        *out_granted_us = (uint32_t)(timer_get(t) / TIMER_100NS_PER_US);
    }
    rc = AXL_OK;
out:
    gBS->RestoreTPL(old);
    if (rc == AXL_OK && mAtexit == 0) {
        mAtexit = axl_atexit(release_own, NULL);
    }
    return rc;
}

int
axl_backend_timer_resolution_end(
    uint32_t id
    )
{
    EFI_TIMER_ARCH_PROTOCOL *t = timer_arch();
    TimerRegistry           *r = registry(false);
    if (id == 0 || t == NULL || r == NULL) {
        return AXL_INVALID;
    }
    EFI_TPL old = gBS->RaiseTPL(TPL_NOTIFY);
    int rc = AXL_INVALID;
    for (uint32_t i = 0; i < TIMER_REG_SLOTS; i++) {
        TimerRequest *q = &r->req[i];
        if (q->id == id && q->owner == (void *)gImageHandle && q->token == own_token()) {
            axl_memset(q, 0, sizeof(*q));
            rc = AXL_OK;
            break;
        }
    }
    if (rc == AXL_OK) {
        prune(r);
        apply(r, t);   /* ending never fails: a refused coarsening leaves it finer */
    }
    gBS->RestoreTPL(old);
    return rc;
}

uint32_t
axl_backend_timer_period_us(void)
{
    EFI_TIMER_ARCH_PROTOCOL *t = timer_arch();
    return t == NULL ? 0 : (uint32_t)(timer_get(t) / TIMER_100NS_PER_US);
}
