/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-tee.h
    Fan one console producer's @ref AxlConsoleOps out to several consumers.

    A producer binds exactly one ops vtable, so "render locally **and** mirror
    remotely" — the obvious shape for a BMC, and for any tool that wants a local
    terminal plus a remote viewer — has no expression in the contract. This is that
    composition:

    @code
    AxlConsoleTee *tee = axl_console_tee_new();

    void *u = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(term, &u);   // local grid
    axl_console_tee_add(tee, ops, u);
    ops = axl_console_vt_enc_ops(enc, &u);                       // remote wire
    axl_console_tee_add(tee, ops, u);

    axl_console_tee_ops(tee, &u);
    axl_console_device_install(axl_console_tee_ops(tee, &u), u, &dcfg, &dev);
    @endcode

    **Why this is substrate and not three lines in each consumer.** Most ops return
    `void` and forwarding them is trivial — but @ref AxlConsoleOps::scrollrect and
    @ref AxlConsoleOps::set_term_prop return **negotiation**, and a consumer may
    decline. Split across several consumers the answers can disagree, and the naive
    "return what the last one said" silently corrupts a grid. The tee answers
    **accepted only when every consumer accepted**; see
    @ref axl_console_tee_ops for why that is the safe direction.

    The tee holds borrowed pointers: every consumer's ops vtable and context must
    outlive it, and it must outlive the producer bound to it. Tear down producer
    first, then tee, then consumers.
**/

#ifndef AXL_CONSOLE_TEE_H
#define AXL_CONSOLE_TEE_H

#include <stddef.h>
#include <axl/axl-macros.h>
#include <axl/axl-console-ops.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum consumers a single tee fans to. */
#define AXL_CONSOLE_TEE_MAX 8

/** @brief Opaque fan-out instance. */
typedef struct AxlConsoleTee AxlConsoleTee;

/**
 * @brief Create an empty tee.
 * @return the instance, or NULL on allocation failure.
 */
AxlConsoleTee *
axl_console_tee_new(void);

/** @brief Destroy a tee. NULL-safe. Does NOT touch the consumers it fanned to. */
void
axl_console_tee_free(
    AxlConsoleTee *t   ///< instance (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlConsoleTee, axl_console_tee_free)
#endif

/**
 * @brief Add a consumer. Consumers are called in the order added.
 *
 * @p ops and @p user are BORROWED — both must outlive the tee.
 *
 * @return AXL_OK, or AXL_ERR on a NULL @p t / @p ops or when the tee already holds
 *     @ref AXL_CONSOLE_TEE_MAX consumers.
 */
int
axl_console_tee_add(
    AxlConsoleTee       *t,     ///< instance
    const AxlConsoleOps *ops,   ///< the consumer's vtable (borrowed)
    void                *user   ///< the consumer's op context (borrowed)
);

/**
 * @brief The vtable to hand to a producer, plus its context.
 *
 * Every op is bound, whatever the consumers bind: an op no consumer implements
 * becomes a no-op (and, for the two negotiated ops, a decline). That matters —
 * @ref AxlConsoleOps::erase must appear bound whenever a scroll can be declined, and
 * with a tee in the middle it always can be.
 *
 * **The negotiated ops answer "accepted" only if EVERY consumer accepted**, and every
 * consumer is asked (no short-circuit), so the answer never depends on the order they
 * were added in. Declining is the safe direction because it is *recoverable*: a
 * producer that is told its scroll was declined falls back to redrawing the rect as
 * ordinary damage, which repairs the consumers that did scroll as well as the ones
 * that did not. The opposite error is not recoverable — reporting "accepted" when one
 * consumer declined means no damage is ever emitted, and that consumer's grid stays
 * wrong with nothing to correct it.
 *
 * @param t    instance.
 * @param user [out] receives the op context (may be NULL).
 * @return the borrowed ops vtable, or NULL when @p t is NULL.
 */
const AxlConsoleOps *
axl_console_tee_ops(
    AxlConsoleTee  *t,     ///< instance
    void          **user   ///< [out] op context (may be NULL)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_TEE_H */
