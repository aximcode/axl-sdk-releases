/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-resolve.c
    DNS4 lookups: axl_net_resolve (forward, name -> IPv4, with IPv4-literal
    fallback) and axl_net_resolve_ptr (reverse, IPv4 -> name).
**/

#include "../backend/axl-backend.h"
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-net.h>

#include "axl-net-internal.h"
#include "../event/axl-cancellable-internal.h"   /* _axl_cancellable_event */

AXL_LOG_DOMAIN("net");

/* ascii_str_to_ucs2 — ASCII to UCS-2 conversion */
static inline EFI_STATUS
ascii_str_to_ucs2(const char *src, unsigned short *dst, size_t dst_max)
{
    size_t i;
    for (i = 0; src[i] != '\0' && i + 1 < dst_max; i++) {
        dst[i] = (unsigned short)(uint8_t)src[i];
    }
    dst[i] = 0;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Shared DNS4 child lifecycle (used by both forward + reverse lookups)
// ---------------------------------------------------------------------------

/* Locate the DNS4 service binding, create a child, get its protocol, and
   configure it (default settings first, falling back to an explicit 8.8.8.8
   resolver when the firmware has no default). On AXL_OK the three out-params
   are a configured DNS4 child the caller drives + closes with dns4_close. */
static int
dns4_open_configured(
    EFI_SERVICE_BINDING_PROTOCOL **out_sb,
    EFI_HANDLE                    *out_child,
    EFI_DNS4_PROTOCOL            **out_dns4)
{
    EFI_HANDLE *handles = NULL;
    size_t      handle_count = 0;

    *out_sb = NULL;
    *out_child = NULL;
    *out_dns4 = NULL;

    EFI_STATUS status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    &gEfiDns4ServiceBindingProtocolGuid,
                    NULL,
                    &handle_count,
                    &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        axl_debug("no DNS4 service binding - cannot resolve");
        return AXL_ERR;
    }

    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    handles[0],
                    &gEfiDns4ServiceBindingProtocolGuid,
                    (void **)&sb);
    axl_backend_free(handles);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    EFI_HANDLE child = NULL;
    status = axl_efi_call(sb->CreateChild, 2, sb, &child);
    if (EFI_ERROR(status)) {
        axl_error("DNS4 CreateChild: %llx", (unsigned long long)status);
        return AXL_ERR;
    }

    EFI_DNS4_PROTOCOL *dns4 = NULL;
    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    child,
                    &gEfiDns4ProtocolGuid,
                    (void **)&dns4);
    if (EFI_ERROR(status)) {
        axl_efi_call(sb->DestroyChild, 2, sb, child);
        return AXL_ERR;
    }

    /* Default settings first; if the firmware has no default resolver,
       fall back to an explicit 8.8.8.8 over UDP. */
    EFI_DNS4_CONFIG_DATA cfg;
    axl_memset(&cfg, 0, sizeof(cfg));
    cfg.UseDefaultSetting = true;
    cfg.RetryInterval     = 3;
    cfg.RetryCount        = 2;

    status = axl_efi_call(dns4->Configure, 2, dns4, &cfg);
    if (EFI_ERROR(status)) {
        EFI_IPv4_ADDRESS server;
        axl_memset(&server, 0, sizeof(server));
        server.Addr[0] = 8;
        server.Addr[1] = 8;
        server.Addr[2] = 8;
        server.Addr[3] = 8;

        cfg.UseDefaultSetting  = false;
        cfg.DnsServerListCount = 1;
        cfg.DnsServerList      = &server;
        cfg.StationIp.Addr[0]  = 0;
        cfg.Protocol           = 17;  // UDP

        status = axl_efi_call(dns4->Configure, 2, dns4, &cfg);
        if (EFI_ERROR(status)) {
            axl_error("DNS4 Configure: %llx", (unsigned long long)status);
            axl_efi_call(sb->DestroyChild, 2, sb, child);
            return AXL_ERR;
        }
    }

    *out_sb = sb;
    *out_child = child;
    *out_dns4 = dns4;
    return AXL_OK;
}

/* Tear down a child opened by dns4_open_configured. */
static void
dns4_close(
    EFI_SERVICE_BINDING_PROTOCOL *sb,
    EFI_HANDLE                    child,
    EFI_DNS4_PROTOCOL            *dns4)
{
    if (dns4 != NULL) {
        axl_efi_call(dns4->Configure, 2, dns4, NULL);
    }
    if (sb != NULL && child != NULL) {
        axl_efi_call(sb->DestroyChild, 2, sb, child);
    }
}

// ---------------------------------------------------------------------------
// axl_net_resolve_async — forward lookup driven on a caller's loop
// ---------------------------------------------------------------------------

/* One resolve in flight. The DNS4 completion token's event, a periodic
   dns4->Poll tick (DNS4 is poll-driven, like TCP/UDP), a deadline timeout, and
   the optional cancel event are all sources on the caller's loop — so the
   resolve progresses with no nested loop, safe from a callback / raised TPL.
   Exactly one of the source callbacks reaches resolve_finish(), which removes
   every source, tears the DNS4 child down, fires the user callback, and frees
   the state — the single-completion discipline (the loop dispatches one event
   per iteration and skips removed sources, so there is no second callback). */
typedef struct {
    EFI_SERVICE_BINDING_PROTOCOL *sb;
    EFI_HANDLE                    child;
    EFI_DNS4_PROTOCOL            *dns4;
    EFI_EVENT                     event;       /* DNS completion (we own it) */
    EFI_DNS4_COMPLETION_TOKEN     token;
    AxlLoop                      *loop;
    AxlSourceId                   event_source;
    AxlSourceId                   poll_source;
    AxlSourceId                   timeout_source;
    AxlSourceId                   cancel_source;
    AxlNetResolveDoneFn           cb;
    void                         *user;
    AxlIPv4Address                literal_addr;  /* IP-literal fast path */
    bool                          pending;       /* HostNameToIp issued, not yet done */
    bool                          finished;      /* single-completion guard */
} ResolveAsync;

/* Remove all sources, abort an in-flight query, free DNS4 + token, free the
   state. Does NOT call the user callback (init-error + post-completion paths). */
static void
resolve_async_destroy(ResolveAsync *s)
{
    axl_loop_remove_source(s->loop, s->event_source);
    axl_loop_remove_source(s->loop, s->poll_source);
    axl_loop_remove_source(s->loop, s->timeout_source);
    axl_loop_remove_source(s->loop, s->cancel_source);

    /* Order matters: abort an in-flight query, then tear the DNS4 child down
       BEFORE closing the completion event or freeing the token (both embedded
       in s). dns4_close's Configure(NULL) resets the instance and, per the
       UEFI DNS4 spec, aborts every pending token and signals its event
       SYNCHRONOUSLY; DestroyChild then releases the instance. So once
       dns4_close returns the firmware no longer references &s->token or
       s->event — we can close/free them with no post-Cancel drain (a blocking
       drain would be illegal here: destroy runs inside a loop callback). */
    if (s->pending && s->dns4 != NULL) {
        axl_efi_call(s->dns4->Cancel, 2, s->dns4, &s->token);
    }
    if (s->token.RspData.H2AData != NULL) {
        if (s->token.RspData.H2AData->IpList != NULL) {
            axl_backend_free(s->token.RspData.H2AData->IpList);
        }
        axl_backend_free(s->token.RspData.H2AData);
    }
    if (s->dns4 != NULL) {
        dns4_close(s->sb, s->child, s->dns4);
    }
    if (s->event != NULL) {
        axl_backend_event_close((AxlEventHandle)s->event);
    }
    axl_free(s);
}

/* The one completion path: copy the (borrowed) address off the state, tear
   everything down, then invoke the user callback. */
static void
resolve_async_finish(ResolveAsync *s, const AxlIPv4Address *addr, AxlStatus st)
{
    if (s->finished) {
        return;
    }
    s->finished = true;

    AxlIPv4Address        tmp;
    const AxlIPv4Address *out  = NULL;
    AxlNetResolveDoneFn   cb   = s->cb;
    void                 *user = s->user;

    if (addr != NULL) {
        tmp = *addr;
        out = &tmp;
    }
    resolve_async_destroy(s);   /* frees s + token; tmp is a stack copy */
    cb(out, st, user);
}

static bool
on_resolve_dns_complete(void *data)
{
    ResolveAsync   *s = (ResolveAsync *)data;
    AxlIPv4Address  addr;
    bool            have = false;

    s->pending = false;   /* the query completed — nothing left to cancel */

    if (!EFI_ERROR(s->token.Status) &&
        s->token.RspData.H2AData != NULL &&
        s->token.RspData.H2AData->IpCount > 0 &&
        s->token.RspData.H2AData->IpList != NULL)
    {
        axl_memcpy(&addr, &s->token.RspData.H2AData->IpList[0],
                   sizeof(EFI_IPv4_ADDRESS));
        have = true;
    }

    resolve_async_finish(s, have ? &addr : NULL, have ? AXL_OK : AXL_ERR);
    return AXL_SOURCE_REMOVE;
}

static bool
on_resolve_dns_poll(void *data)
{
    ResolveAsync *s = (ResolveAsync *)data;
    axl_efi_call(s->dns4->Poll, 1, s->dns4);
    return AXL_SOURCE_CONTINUE;
}

static bool
on_resolve_timeout(void *data)
{
    resolve_async_finish((ResolveAsync *)data, NULL, AXL_TIMEOUT);
    return AXL_SOURCE_REMOVE;
}

static bool
on_resolve_cancel(void *data)
{
    resolve_async_finish((ResolveAsync *)data, NULL, AXL_CANCELLED);
    return AXL_SOURCE_REMOVE;
}

/* IP-literal fast path: deliver the parsed address via a 0-delay timeout so the
   callback is always deferred (never re-entrant from axl_net_resolve_async). */
static bool
on_resolve_literal(void *data)
{
    ResolveAsync *s = (ResolveAsync *)data;
    resolve_async_finish(s, &s->literal_addr, AXL_OK);
    return AXL_SOURCE_REMOVE;
}

int
axl_net_resolve_async(
    const char       *hostname,
    AxlLoop          *loop,
    AxlCancellable   *cancel,
    AxlNetResolveDoneFn cb,
    void             *user)
{
    if (hostname == NULL || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    ResolveAsync *s = axl_malloc(sizeof(*s));
    if (s == NULL) {
        return AXL_ERR;
    }
    axl_memset(s, 0, sizeof(*s));
    s->loop = loop;
    s->cb   = cb;
    s->user = user;

    /* IPv4 literal? deliver it deferred (1 ms — axl_loop_add_timeout rejects a
       0 delay; the point is only that the callback never fires re-entrantly
       from this call), no DNS. */
    if (!EFI_ERROR(net_parse_ip_address(hostname,
                                        (EFI_IPv4_ADDRESS *)&s->literal_addr))) {
        s->timeout_source = axl_loop_add_timeout(loop, 1, on_resolve_literal, s);
        if (s->timeout_source == 0) {
            axl_free(s);
            return AXL_ERR;
        }
        return AXL_OK;
    }

    if (dns4_open_configured(&s->sb, &s->child, &s->dns4) != AXL_OK) {
        axl_free(s);
        return AXL_ERR;
    }

    if (axl_backend_event_create((AxlEventHandle *)&s->event) != AXL_OK) {
        dns4_close(s->sb, s->child, s->dns4);
        axl_free(s);
        return AXL_ERR;
    }

    s->token.Event  = s->event;
    s->token.Status = EFI_ABORTED;

    size_t          host_len = axl_strlen(hostname);
    unsigned short *host_w   =
        axl_backend_alloc((host_len + 1) * sizeof(unsigned short));
    if (host_w == NULL) {
        resolve_async_destroy(s);
        return AXL_ERR;
    }
    ascii_str_to_ucs2(hostname, host_w, host_len + 1);
    EFI_STATUS status = axl_efi_call(s->dns4->HostNameToIp, 3,
                                     s->dns4, host_w, &s->token);
    axl_backend_free(host_w);
    if (EFI_ERROR(status)) {
        axl_error("DNS4 HostNameToIp: %llx", (unsigned long long)status);
        resolve_async_destroy(s);
        return AXL_ERR;
    }
    s->pending = true;

    s->event_source = axl_loop_add_event(loop, (void *)s->event,
                                         on_resolve_dns_complete, s);
    /* 10 ms dns4->Poll tick — DNS4 is poll-driven (AXL_NET_POLL_TICK_US). */
    s->poll_source    = axl_loop_add_timer(loop, 10, on_resolve_dns_poll, s);
    s->timeout_source = axl_loop_add_timeout(loop, 5000, on_resolve_timeout, s);
    if (s->event_source == 0 || s->poll_source == 0 || s->timeout_source == 0) {
        resolve_async_destroy(s);   /* removes whatever registered + cancels */
        return AXL_ERR;
    }
    if (cancel != NULL) {
        s->cancel_source = axl_loop_add_event(loop, _axl_cancellable_event(cancel),
                                              on_resolve_cancel, s);
        /* Non-fatal: the resolve just runs uncancellable if this fails. */
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_net_resolve — forward lookup (hostname -> IPv4); sync wrapper over async
// ---------------------------------------------------------------------------

typedef struct {
    AxlIPv4Address addr;
    AxlStatus      st;
    AxlLoop       *loop;
} ResolveSyncResult;

static void
on_resolve_sync(const AxlIPv4Address *addr, AxlStatus st, void *user)
{
    ResolveSyncResult *r = (ResolveSyncResult *)user;
    if (addr != NULL) {
        r->addr = *addr;
    }
    r->st = st;
    axl_loop_quit(r->loop);
}

int
axl_net_resolve(const char *hostname, AxlIPv4Address *addr)
{
    if (hostname == NULL || addr == NULL) {
        return AXL_ERR;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    ResolveSyncResult r = { .st = AXL_ERR, .loop = loop };
    if (axl_net_resolve_async(hostname, loop, NULL, on_resolve_sync, &r) != AXL_OK) {
        axl_loop_free(loop);
        return AXL_ERR;
    }
    axl_loop_run(loop);
    axl_loop_free(loop);

    if (r.st == AXL_OK) {
        *addr = r.addr;
        return AXL_OK;
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// axl_net_resolve_ptr — reverse lookup (IPv4 -> hostname)
// ---------------------------------------------------------------------------

int
axl_net_resolve_ptr(const AxlIPv4Address *ip, char *out, size_t cap)
{
    EFI_SERVICE_BINDING_PROTOCOL *sb;
    EFI_HANDLE                    child;
    EFI_DNS4_PROTOCOL            *dns4;
    EFI_EVENT                     dns_event;
    EFI_DNS4_COMPLETION_TOKEN     dns_token;
    EFI_IPv4_ADDRESS              query;
    EFI_STATUS                    status;

    if (ip == NULL || out == NULL || cap == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';

    if (dns4_open_configured(&sb, &child, &dns4) != AXL_OK) {
        return AXL_ERR;
    }

    dns_event = NULL;
    if (axl_backend_event_create((AxlEventHandle *)&dns_event) != AXL_OK) {
        dns4_close(sb, child, dns4);
        return AXL_ERR;
    }

    axl_memset(&dns_token, 0, sizeof(dns_token));
    dns_token.Event  = dns_event;
    dns_token.Status = EFI_ABORTED;

    /* EFI_IPv4_ADDRESS and AxlIPv4Address are both a bare 4-octet array;
       IpToHostName takes the address by value (the firmware builds the
       in-addr.arpa PTR query). */
    axl_memcpy(&query, ip->addr, sizeof(query));

    status = axl_efi_call(dns4->IpToHostName, 3, dns4, query, &dns_token);
    if (EFI_ERROR(status)) {
        axl_error("DNS4 IpToHostName: %llx", (unsigned long long)status);
        axl_backend_event_close((AxlEventHandle)dns_event);
        dns4_close(sb, child, dns4);
        return AXL_ERR;
    }

    bool dns_completed = (_axl_dns_wait(dns4, dns_event, 5000000) == 0);
    if (!dns_completed) {
        axl_efi_call(dns4->Cancel, 2, dns4, &dns_token);
        _axl_dns_wait(dns4, dns_event, 100 * 1000);
    }

    int rc = AXL_ERR;
    if (!EFI_ERROR(dns_token.Status) &&
        dns_token.RspData.A2HData != NULL &&
        dns_token.RspData.A2HData->HostName != NULL)
    {
        /* HostName is a UCS-2 string the firmware allocated; decode to the
           caller's UTF-8 buffer. A non-empty name is the success signal. */
        axl_ucs2_to_utf8_buf(
            (const unsigned short *)dns_token.RspData.A2HData->HostName,
            out, cap);
        if (out[0] != '\0') {
            rc = AXL_OK;
        }
    }

    if (dns_token.RspData.A2HData != NULL) {
        if (dns_token.RspData.A2HData->HostName != NULL) {
            axl_backend_free(dns_token.RspData.A2HData->HostName);
        }
        axl_backend_free(dns_token.RspData.A2HData);
    }

    axl_backend_event_close((AxlEventHandle)dns_event);
    dns4_close(sb, child, dns4);
    return rc;
}
