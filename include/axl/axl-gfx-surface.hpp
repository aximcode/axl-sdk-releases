/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-surface.hpp
 *
 * Scoped redirection of the graphics draw target, so a nested render restores
 * what it interrupted instead of resetting to the screen.
 *
 * @code
 * #include <axl/axl-gfx-surface.hpp>
 *
 * void Widget::render(AxlGfxBuffer *back)
 * {
 *     axl::gfx_target_scope target{back};   // saves the caller's, installs back
 *     axl_gfx_fill_rect(...);               // draws into back
 *     for (Widget *c : children_) {
 *         c->render(c->buffer());           // nests; restores to back on exit
 *     }
 * }                                         // restores the caller's target
 * @endcode
 *
 * @par Save-and-restore, not set-and-clear
 *
 * axl_gfx_target_buffer() sets a single global target and
 * axl_gfx_get_current_target() reads it, which makes the naive guard "set on
 * entry, `axl_gfx_target_buffer(NULL)` on exit" — and that is wrong the moment
 * anything nests. `NULL` is not "no target", it is *the screen*, so an inner
 * scope resetting to it silently redirects the rest of an outer widget's
 * painting from its back buffer onto the display: tearing, or drawing that
 * lands somewhere nothing composites. This captures the target that was
 * active at construction and puts THAT back.
 *
 * A NULL @a buffer is a legitimate argument and means "draw to the screen",
 * so a headless path needs no special case.
 *
 * @par Why this is in the SDK and not left to the consumer
 *
 * It already existed, hand-written, in the only C++ consumer — a
 * `saved_target_` member with a two-line constructor and destructor.
 * AXL-Cxx-Design.md §1 counted that as the demand signal for this whole layer
 * and drew the conclusion directly: when your only C++ consumer has already
 * invented a mechanism by hand, the mechanism belongs in the foundation.
 *
 * @par Concrete, not a generic scope guard
 *
 * There is no `axl::scope_guard<T>` here and this does not use one. The whole
 * public API has exactly one save-and-restore-a-global pair, so a template
 * would be an abstraction with a single caller — and a generic guard has to be
 * told the getter, the setter and the value, which is longer at the call site
 * than the thing it abstracts. If a second such API ever appears, two concrete
 * guards are still the cheaper answer.
 *
 * @par Neither copyable nor movable
 *
 * The restore must happen exactly once, at the end of the scope that
 * redirected. A movable guard could outlive its scope or be dropped early,
 * which is a rendering bug with no diagnostic. `std::unique_ptr` is where you
 * go when ownership needs to travel; a target scope never does.
 */

#ifndef AXL_GFX_SURFACE_HPP
#define AXL_GFX_SURFACE_HPP

#ifndef __cplusplus
#error "axl-gfx-surface.hpp is C++ only; C consumers want axl-gfx-surface.h"
#endif

#include <axl/axl-gfx-surface.h>

namespace axl {

/**
 * Redirects the draw target for the lifetime of the object, then restores
 * whatever was active before it.
 *
 * @warning The SAVED target must outlive the scope. This holds a private copy
 *     of the outgoing `AxlGfxBuffer *`, which `axl_gfx_buffer_free()` cannot
 *     see — its own defensive `if (target_buf == buf) target_buf = NULL;`
 *     fires only for the target currently INSTALLED, and by construction that
 *     is never the saved one. So freeing an outer buffer while an inner scope
 *     is open (a widget resizing its backing store during a child's render)
 *     makes the restore install a dangling pointer, and every later draw
 *     writes into freed heap. Reorder the free to after the scope closes, or
 *     close the scope first.
 *
 * @warning The CLIP STACK is a second global and is not saved. Clips apply in
 *     buffer-local coordinates (see axl_gfx_target_buffer()), so a clip
 *     pushed against the outer buffer stays active across a nested scope and
 *     is silently reinterpreted in the inner buffer's coordinate space. Push
 *     clips after entering a scope and pop them before leaving it.
 *
 * Marked `[[nodiscard]]` on the class so that forgetting the variable name —
 * `axl::gfx_target_scope{back};`, which constructs and destroys inside the
 * one full-expression and redirects nothing — is a diagnostic rather than a
 * rendering bug with no symptom. It is the `std::lock_guard` mistake, and
 * this class exists to prevent exactly that category.
 */
class [[nodiscard]] gfx_target_scope {
public:
    /**
     * Capture the current target and install @a buffer in its place.
     *
     * The capture happens BEFORE the install, so a nested scope saves its
     * caller's buffer rather than its own.
     */
    explicit gfx_target_scope(
        AxlGfxBuffer *buffer    ///< buffer to draw into, or NULL for the screen
    ) noexcept
        : m_saved(axl_gfx_get_current_target())
    {
        axl_gfx_target_buffer(buffer);
    }

    /// Restore the target that was active at construction.
    ~gfx_target_scope() { axl_gfx_target_buffer(m_saved); }

    gfx_target_scope(const gfx_target_scope &) = delete;
    gfx_target_scope &operator=(const gfx_target_scope &) = delete;
    gfx_target_scope(gfx_target_scope &&) = delete;
    gfx_target_scope &operator=(gfx_target_scope &&) = delete;

    /**
     * The target this scope will restore.
     *
     * The OUTER target, not the one currently installed — which is what a
     * nested renderer needs when it has to know what it interrupted (to
     * composite into it, or to decide it is drawing to the screen after all).
     *
     * @return the buffer active at construction, or NULL if that was the
     *     screen.
     */
    [[nodiscard]] AxlGfxBuffer *saved() const noexcept { return m_saved; }

private:
    AxlGfxBuffer *m_saved;
};

} // namespace axl

#endif /* AXL_GFX_SURFACE_HPP */
