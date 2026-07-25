/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-attempt.c
    Crash-safe attempt engine — breadcrumb, quarantine, bounded result log.

    Every list is a '\n'-separated NUL-terminated string in one
    non-volatile variable. That shape is deliberate: axl_nvstore_get
    hands back (buf, &size), so a packed buffer needs no conversion in
    either direction, and the whole list is one atomic firmware write.

    Bounds are hard, not advisory. axl_nvstore_get fails outright on a
    value larger than the caller's buffer rather than truncating, so a
    list that grows past its bound would become unreadable — and an
    unreadable quarantine list is worse than a short one. Hence the
    drop-oldest trim in list_append and the up-front refusal in
    axl_attempt_begin.

    Writes are best-effort throughout: read-only or full NVRAM warns and
    continues. The durable record is a convenience for the next boot; a
    caller that cannot write one still has a working run in front of it.
**/

#include <axl/axl-attempt.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("attempt");

/* Pure: is @name one of the '\n'-separated entries in @list? */
static bool
list_contains(
    const char *list,
    const char *name
    )
{
    size_t nlen = axl_strlen(name);
    const char *p = list;
    while (*p != '\0') {
        const char *nl = axl_strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : axl_strlen(p);
        if (seg == nlen && axl_memcmp(p, name, nlen) == 0) {
            return true;
        }
        p = nl ? nl + 1 : p + seg;
    }
    return false;
}

/* Pure: append "@name\n" to @list (in @cap bytes), dedup; drop oldest line if
   it would overflow. */
static void
list_push(
    char       *list,
    size_t      cap,
    const char *name
    )
{
    if (list_contains(list, name)) {
        return;
    }
    size_t need = axl_strlen(name) + 1;   /* name + '\n' */
    size_t have = axl_strlen(list);
    while (have + need + 1 > cap && have > 0) {   /* drop oldest (first line) */
        char *nl = axl_strchr(list, '\n');
        if (nl == NULL) { list[0] = '\0'; have = 0; break; }
        size_t rest = axl_strlen(nl + 1);
        axl_memmove(list, nl + 1, rest + 1);
        have = rest;
    }
    axl_snprintf(list + have, cap - have, "%s\n", name);
}

/* Shared get-append-set: read the '\n'-separated NV list at @key (up to
   @cap bytes), append @line (dedup + drop-oldest per list_push), and write
   it back. */
static void
list_append(
    const AxlAttempt *at,
    const char       *key,
    size_t            cap,
    const char       *line
    )
{
    char *buf = (char *)axl_calloc(1, cap);
    if (buf == NULL) {
        axl_warning("out of memory appending to %s", key);
        return;
    }
    size_t sz = cap;
    axl_nvstore_get(at->ns, key, buf, &sz);   /* empty if absent */
    buf[cap - 1] = '\0';
    list_push(buf, cap, line);
    /* Best-effort durability: warn on write failure (read-only/full NVRAM) but
       continue — the NVRAM record is a convenience, never a hard dependency. */
    if (axl_nvstore_set(at->ns, key, buf, axl_strlen(buf) + 1,
                        at->flags) != AXL_OK) {
        axl_warning("could not persist %s (read-only/full NVRAM?)", key);
    }
    axl_free(buf);
}

/* Shared read-whole-list: true iff @key holds a non-empty value that fits
   in @cap. */
static bool
list_read(
    const AxlAttempt *at,
    const char       *key,
    char             *out,
    size_t            cap
    )
{
    size_t sz = cap;
    out[0] = '\0';
    if (axl_nvstore_get(at->ns, key, out, &sz) != AXL_OK) {
        return false;
    }
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

int
axl_attempt_init(
    AxlAttempt    *at,
    const char    *ns,
    const AxlGuid *vendor
    )
{
    if (at == NULL) {
        return AXL_ERR;   /* nothing to make inert */
    }
    /* Disarm and fill the descriptor to a safe inert state BEFORE validating
       anything. A caller is entitled to ignore the return (every operation is
       best-effort by design), so a stack AxlAttempt must never be left holding
       garbage on ANY error path — the one thing this module must not do is
       fault the box. `valid` gates every operation, so an init that fails for
       any reason (NULL argument or registration failure) yields a descriptor
       whose begin/end/etc. are clean no-ops. `ns` is set NULL as belt-and-
       suspenders — an inert op never reaches nvstore, but a NULL ns must not
       be the sentinel doing the gating, because ns_to_guid(NULL) resolves to
       the GLOBAL namespace and would silently misdirect a write there. */
    at->valid          = false;
    at->ns             = NULL;
    at->trying_key     = "Trying";
    at->quarantine_key = "Quarantine";
    at->log_key        = "Log";
    at->name_max       = AXL_ATTEMPT_NAME_MAX;
    at->quarantine_max = AXL_ATTEMPT_QUARANTINE_MAX;
    at->log_max        = AXL_ATTEMPT_LOG_MAX;
    at->flags          = AXL_NV_PERSISTENT | AXL_NV_BOOT;
    if (ns == NULL || vendor == NULL) {
        return AXL_ERR;
    }
    if (axl_nvstore_register_namespace(ns, vendor) != AXL_OK) {
        return AXL_ERR;
    }
    at->ns    = ns;
    at->valid = true;
    return AXL_OK;
}

bool
axl_attempt_begin(
    const AxlAttempt *at,
    const char       *name
    )
{
    if (at == NULL || !at->valid || name == NULL) {
        return false;
    }
    /* Don't write a breadcrumb we can't read back: axl_attempt_pending reads
       into a bounded buffer and axl_nvstore_get returns AXL_ERR (not a
       truncated read) when the value is larger, so an over-long name would
       leave a dangling breadcrumb that crash-recovery can never see or clear. */
    if (axl_strlen(name) + 1 > at->name_max) {
        axl_warning("name too long for breadcrumb (>= %zu bytes); skipping",
                    at->name_max);
        return false;
    }
    if (axl_nvstore_set(at->ns, at->trying_key, name, axl_strlen(name) + 1,
                        at->flags) != AXL_OK) {
        axl_warning("could not persist breadcrumb (read-only/full NVRAM?)");
        return false;
    }
    return true;
}

void
axl_attempt_end(
    const AxlAttempt *at
    )
{
    if (at == NULL || !at->valid) {
        return;
    }
    axl_nvstore_delete(at->ns, at->trying_key);
}

bool
axl_attempt_pending(
    const AxlAttempt *at,
    char             *out,
    size_t            cap
    )
{
    if (at == NULL || !at->valid || out == NULL || cap == 0) {
        return false;
    }
    size_t sz = cap;
    if (axl_nvstore_get(at->ns, at->trying_key, out, &sz) != AXL_OK || sz == 0) {
        return false;
    }
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

int
axl_attempt_recover(
    const AxlAttempt *at,
    char             *out,
    size_t            cap
    )
{
    if (at == NULL || !at->valid || out == NULL || cap == 0) {
        return AXL_ERR;
    }
    if (!axl_attempt_pending(at, out, cap)) {
        return 0;
    }
    axl_attempt_quarantine(at, out);
    axl_attempt_end(at);
    return 1;
}

void
axl_attempt_quarantine(
    const AxlAttempt *at,
    const char       *name
    )
{
    if (at == NULL || !at->valid || name == NULL) {
        return;
    }
    list_append(at, at->quarantine_key, at->quarantine_max, name);
}

bool
axl_attempt_is_quarantined(
    const AxlAttempt *at,
    const char       *name
    )
{
    if (at == NULL || !at->valid || name == NULL) {
        return false;
    }
    char *q = (char *)axl_calloc(1, at->quarantine_max);
    if (q == NULL) {
        /* Fail CLOSED. If the list cannot be read, the engine does not know
           whether @name wedged the box last boot — and the whole point of the
           module is to not find out the hard way. Reporting "quarantined"
           costs a skipped candidate; reporting "clear" re-enters the boot loop
           this exists to break. */
        axl_warning("out of memory reading %s; treating '%s' as quarantined",
                    at->quarantine_key, name);
        return true;
    }
    size_t sz = at->quarantine_max;
    bool found = false;
    if (axl_nvstore_get(at->ns, at->quarantine_key, q, &sz) == AXL_OK) {
        q[at->quarantine_max - 1] = '\0';
        found = list_contains(q, name);
    }
    axl_free(q);
    return found;
}

bool
axl_attempt_quarantine_read(
    const AxlAttempt *at,
    char             *out,
    size_t            cap
    )
{
    if (at == NULL || !at->valid || out == NULL || cap == 0) {
        return false;
    }
    return list_read(at, at->quarantine_key, out, cap);
}

void
axl_attempt_log(
    const AxlAttempt *at,
    const char       *line
    )
{
    if (at == NULL || !at->valid || line == NULL) {
        return;
    }
    list_append(at, at->log_key, at->log_max, line);
}

bool
axl_attempt_log_read(
    const AxlAttempt *at,
    char             *out,
    size_t            cap
    )
{
    if (at == NULL || !at->valid || out == NULL || cap == 0) {
        return false;
    }
    return list_read(at, at->log_key, out, cap);
}

void
axl_attempt_clear(
    const AxlAttempt *at
    )
{
    if (at == NULL || !at->valid) {
        return;
    }
    axl_nvstore_delete(at->ns, at->trying_key);
    axl_nvstore_delete(at->ns, at->quarantine_key);
    axl_nvstore_delete(at->ns, at->log_key);
}
