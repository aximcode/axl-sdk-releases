/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-attempt.h
    Crash-safe attempt engine — breadcrumb, quarantine, bounded result log.

    For operations that can hang or fault the box outright: loading an
    arbitrary firmware image, initializing an option ROM, staging a SPI
    update. The hazard is that the failure takes the whole machine with
    it — no exception fires, no handler runs, nothing is written down,
    and the next boot repeats the identical sequence and hangs
    identically. That is an unbreakable boot loop with no record of what
    caused it.

    The engine breaks the loop with a durable breadcrumb:

    1. Before the risky step, axl_attempt_begin() writes the name of the
       thing about to be tried to non-volatile storage.
    2. If the step returns, axl_attempt_end() erases the breadcrumb.
    3. If the box hangs or resets, the breadcrumb survives. On the next
       boot axl_attempt_recover() finds it: a breadcrumb that outlived
       its attempt means that attempt never completed, so the named
       thing is the culprit. It is added to a persistent quarantine list
       and the breadcrumb is cleared.
    4. axl_attempt_is_quarantined() lets the next sweep skip it, so the
       run makes progress instead of wedging in the same place forever.

    A bounded, append-only result log (axl_attempt_log()) records
    outcomes across boots. Log lines are opaque to the engine — the
    caller owns the vocabulary.

    This is complementary to `<axl/axl-crashrecord.h>`, not an
    alternative to it. AxlCrashRecord dumps registers and a stack trace
    when an *exception* fires, which is far richer than a name — but a
    hang raises no exception and a reset runs no handler, so nothing is
    captured. The breadcrumb covers exactly the case a crash handler
    cannot see, and the two coexist.

    Namespacing: the engine is storage-shaped, not policy-shaped: the caller supplies
    the AxlNvstore namespace, the vendor GUID that namespace binds to,
    the key names, and the size bounds. Nothing is chosen for you, so two
    independent consumers on the same box cannot collide, and a consumer
    with existing on-disk state can name it exactly and keep it.

    @code
    static const AxlGuid MY_GUID =
        AXL_GUID(0x11223344, 0x5566, 0x7788, 0x99, 0xaa, ...);

    AxlAttempt at;
    axl_attempt_init(&at, "myloader", &MY_GUID);

    // Heal a prior crash before doing anything else.
    char culprit[64];
    if (axl_attempt_recover(&at, culprit, sizeof culprit) == 1) {
        axl_printf("last run died on %s -- quarantined\n", culprit);
    }

    for (size_t i = 0; i < n; i++) {
        if (axl_attempt_is_quarantined(&at, name[i])) {
            continue;                     // known-bad; skip it
        }
        axl_attempt_begin(&at, name[i]);  // breadcrumb BEFORE the hazard
        int rc = load_the_risky_thing(name[i]);
        axl_attempt_end(&at);             // survived
        axl_attempt_log(&at, rc == 0 ? "OK" : "FAIL");
    }
    @endcode
**/

#ifndef AXL_ATTEMPT_H
#define AXL_ATTEMPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#include <axl/axl-sys.h>   /* AxlGuid — the namespace's vendor token */

#ifdef __cplusplus
extern "C" {
#endif

#define AXL_ATTEMPT_NAME_MAX        64     ///< default AxlAttempt.name_max
#define AXL_ATTEMPT_QUARANTINE_MAX  1024   ///< default AxlAttempt.quarantine_max
#define AXL_ATTEMPT_LOG_MAX         2048   ///< default AxlAttempt.log_max

/**
 * @brief Where one consumer's attempt state lives, and how big it may get.
 *
 * Fill via axl_attempt_init(), then override any of the caller-owned
 * fields before first use. The strings are stored as pointers, not
 * copied, and must outlive the AxlAttempt.
 *
 * The three lists are stored as newline-separated NUL-terminated
 * strings in three separate non-volatile variables. Sizing matters:
 * a value larger than its bound cannot be read back at all (the
 * underlying read fails rather than truncating), so lists are trimmed
 * oldest-line-first to stay under the bound, and a name that does not
 * fit in @c name_max is refused rather than written.
 *
 * @c valid is owned by axl_attempt_init, not the caller: it is the
 * armed bit. A descriptor whose init failed (or that was never
 * init'd) has @c valid == false, and every operation treats such a
 * descriptor as inert — a clean no-op, never a wild dereference and
 * never a write to the wrong namespace. Do not set it by hand.
 */
typedef struct {
    const char *ns;               ///< AxlNvstore namespace name (not copied)
    const char *trying_key;       ///< variable name for the breadcrumb (not copied)
    const char *quarantine_key;   ///< variable name for the quarantine list (not copied)
    const char *log_key;          ///< variable name for the result log (not copied)
    size_t      name_max;         ///< bound on one name, including its NUL
    size_t      quarantine_max;   ///< bound on the whole quarantine variable, in bytes
    size_t      log_max;          ///< bound on the whole log variable, in bytes
    uint32_t    flags;            ///< AXL_NV_* flags every write uses
    bool        valid;            ///< armed bit — set by axl_attempt_init, not the caller
} AxlAttempt;

/**
 * @brief Bind @p at to a namespace and fill in the defaults.
 *
 * Registers @p ns with @p vendor via axl_nvstore_register_namespace()
 * and populates @p at with the default key names (`"Trying"`,
 * `"Quarantine"`, `"Log"`), the default bounds
 * (@c AXL_ATTEMPT_NAME_MAX / @c AXL_ATTEMPT_QUARANTINE_MAX /
 * @c AXL_ATTEMPT_LOG_MAX), and persistent + boot-services write flags.
 * Override any field afterwards; a consumer with state already on disk
 * should set the keys and bounds explicitly so its format is pinned at
 * the call site rather than inherited from a default that could move.
 *
 * @p ns and @p vendor are stored by pointer and must outlive @p at.
 *
 * On any failure the descriptor is left INERT, not garbage: @c valid
 * is cleared and @c ns set NULL, so a caller that ignores the return
 * (permitted — every operation is best-effort) and goes on to call
 * axl_attempt_begin/end gets a clean no-op rather than a fault. Only
 * a NULL @p at cannot be made inert; that alone leaves nothing to
 * write.
 *
 * @return AXL_OK on success, AXL_ERR if a parameter is NULL or the
 *     namespace could not be registered.
 */
int
axl_attempt_init(
    AxlAttempt    *at,      ///< [out] descriptor to fill
    const char    *ns,      ///< AxlNvstore namespace name (not copied)
    const AxlGuid *vendor   ///< vendor GUID the namespace binds to (not copied)
);

/**
 * @brief Write the breadcrumb naming what is about to be attempted.
 *
 * Call immediately before the step that may hang or fault. If control
 * never returns, this name is what identifies the culprit on the next
 * boot.
 *
 * A name that does not fit in @c at->name_max is refused with a
 * warning and not written: axl_attempt_pending() reads into a bounded
 * buffer, so an over-long breadcrumb would be one recovery can never
 * see or clear. A write failure (read-only or full non-volatile
 * storage) also warns. Both return false — the caller may proceed
 * unprotected or bail, but should not treat the attempt as
 * breadcrumbed.
 *
 * @return true if a breadcrumb is committed, false if none was written.
 */
bool
axl_attempt_begin(
    const AxlAttempt *at,    ///< engine descriptor
    const char       *name   ///< what is about to be attempted (UTF-8)
);

/**
 * @brief Erase the breadcrumb — the attempt returned.
 *
 * Call once control comes back from the risky step, whether it
 * succeeded or failed cleanly: either way the box survived, so there
 * is no culprit to name. Where exactly this lands relative to any
 * follow-on work is a real decision — anything still inside the
 * breadcrumb window is attributed to @a name if the box dies there.
 *
 * Idempotent; harmless with no breadcrumb outstanding.
 */
void
axl_attempt_end(
    const AxlAttempt *at   ///< engine descriptor
);

/**
 * @brief Read the outstanding breadcrumb, if any.
 *
 * A breadcrumb present at the start of a run is one that outlived its
 * attempt: the box died before axl_attempt_end() ran. Pure query —
 * nothing is quarantined or cleared. Use it when the recovery policy
 * (what to print, what to log, in what order) is the caller's;
 * axl_attempt_recover() is the ready-made version.
 *
 * @p out is always NUL-terminated on a true return.
 *
 * @return true if a non-empty breadcrumb was read into @p out, false
 *     otherwise.
 */
bool
axl_attempt_pending(
    const AxlAttempt *at,   ///< engine descriptor
    char             *out,  ///< [out] culprit name buffer
    size_t            cap   ///< size of @p out in bytes
);

/**
 * @brief Heal a prior crash: quarantine the culprit and clear the breadcrumb.
 *
 * The common composition of axl_attempt_pending() +
 * axl_attempt_quarantine() + axl_attempt_end(). Call once at the start
 * of a run, before attempting anything, so a name that wedged the box
 * last boot is on the quarantine list before this boot reaches it.
 *
 * Nothing is printed and nothing is logged — the outcome vocabulary is
 * the caller's. A caller that wants to log the recovery with its own
 * token, or interleave the steps differently, should compose the three
 * primitives instead.
 *
 * @return 1 if a crash was recovered and @p out holds the culprit, 0 if
 *     the previous run completed cleanly (no breadcrumb), AXL_ERR on a
 *     bad parameter.
 */
int
axl_attempt_recover(
    const AxlAttempt *at,   ///< engine descriptor
    char             *out,  ///< [out] culprit name buffer
    size_t            cap   ///< size of @p out in bytes
);

/**
 * @brief Add @p name to the persistent quarantine list.
 *
 * Duplicates are ignored. If the list would exceed
 * @c at->quarantine_max the oldest entries are dropped to make room,
 * so quarantining never fails for want of space — it forgets instead.
 * Best-effort: a write failure warns and continues, since the record
 * is a convenience and never a hard dependency.
 */
void
axl_attempt_quarantine(
    const AxlAttempt *at,    ///< engine descriptor
    const char       *name   ///< name to quarantine (UTF-8)
);

/**
 * @brief Is @p name on the quarantine list?
 *
 * Fails closed on allocation failure: if the scratch buffer for the
 * list cannot be allocated, @p name is reported as quarantined. The
 * engine cannot know whether @p name wedged the box last boot, and the
 * safe answer to "should I try this?" when it cannot check is no — a
 * skipped candidate costs one sweep, a wrong "clear" costs the boot
 * loop this module exists to break. (An absent or empty list, by
 * contrast, is a successful read of "nothing quarantined" and correctly
 * returns false — a name is not quarantined until it is listed.)
 *
 * @return true if @p name was previously quarantined and has not been
 *     aged out or cleared, or if the scratch buffer could not be
 *     allocated; false otherwise, including an inert descriptor.
 */
AXL_WARN_UNUSED bool
axl_attempt_is_quarantined(
    const AxlAttempt *at,    ///< engine descriptor
    const char       *name   ///< name to test (UTF-8)
);

/**
 * @brief Read the whole quarantine list as one newline-separated string.
 *
 * Renders the raw stored value. @p out is NUL-terminated on a true
 * return.
 *
 * @return true if a non-empty list was read into @p out, false if the
 *     list is empty, absent, or larger than @p cap.
 */
bool
axl_attempt_quarantine_read(
    const AxlAttempt *at,   ///< engine descriptor
    char             *out,  ///< [out] buffer
    size_t            cap   ///< size of @p out in bytes
);

/**
 * @brief Append one line to the bounded, append-only result log.
 *
 * @p line is opaque to the engine — it is the caller's outcome
 * vocabulary, and the caller renders it back. Duplicate lines are
 * ignored, and the oldest lines are dropped when the log would exceed
 * @c at->log_max, so the log is a bounded record of distinct recent
 * outcomes rather than a complete history. Best-effort: a write
 * failure warns and continues.
 */
void
axl_attempt_log(
    const AxlAttempt *at,    ///< engine descriptor
    const char       *line   ///< line to append (UTF-8, no embedded newline)
);

/**
 * @brief Read the whole result log as one newline-separated string.
 *
 * @p out is NUL-terminated on a true return.
 *
 * @return true if a non-empty log was read into @p out, false if the
 *     log is empty, absent, or larger than @p cap.
 */
bool
axl_attempt_log_read(
    const AxlAttempt *at,   ///< engine descriptor
    char             *out,  ///< [out] buffer
    size_t            cap   ///< size of @p out in bytes
);

/**
 * @brief Delete the breadcrumb, the quarantine list, and the result log.
 *
 * The operator's "forget everything, start over" — including
 * un-quarantining names a previous boot blamed. Only the engine's own
 * three keys are removed; any other variable in the namespace is left
 * alone.
 */
void
axl_attempt_clear(
    const AxlAttempt *at   ///< engine descriptor
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ATTEMPT_H */
