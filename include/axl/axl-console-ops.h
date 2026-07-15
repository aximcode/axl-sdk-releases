/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-ops.h
    The producer-agnostic console contract: the structured operations a console
    producer reports to a consumer, plus the value types and key-shift-state
    aliases they share. Producers: `axl-console-tap` (UEFI SIMPLE_TEXT_OUTPUT,
    swap strategy), `axl-console-device` (take-over strategy), `axl-vterm` (a real
    VT byte stream). A consumer bound to one binds any of them unchanged.
    See `AXL-Console-Design.md`.
**/

#ifndef AXL_CONSOLE_OPS_H
#define AXL_CONSOLE_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name KeyShiftState bits for a producer's inject_key_ex
 *
 * AXL-owned aliases for the `EFI_KEY_STATE.KeyShiftState` bits a consumer OR's
 * into a producer's `inject_key_ex` (the tap's or the device's) @c shift_state.
 * They exist so a tool
 * that includes only `<axl.h>` can name the modifier a guest's key-notify matches
 * against (the Shell's Ctrl+C, most notably) without reaching into the EDK2/UEFI
 * headers — no UEFI symbol leaks through the AXL public API. The values are the
 * UEFI 2.11 §12.2.5 constants; a unit test pins each alias to its `EFI_*`
 * counterpart so the two cannot drift.
 * @{
 */
#define AXL_CONSOLE_SHIFT_STATE_VALID      0x80000000u ///< the KeyShiftState field is valid
#define AXL_CONSOLE_RIGHT_SHIFT_PRESSED    0x00000001u
#define AXL_CONSOLE_LEFT_SHIFT_PRESSED     0x00000002u
#define AXL_CONSOLE_RIGHT_CONTROL_PRESSED  0x00000004u
#define AXL_CONSOLE_LEFT_CONTROL_PRESSED   0x00000008u
#define AXL_CONSOLE_RIGHT_ALT_PRESSED      0x00000010u
#define AXL_CONSOLE_LEFT_ALT_PRESSED       0x00000020u
/** @} */

/**
 * @brief How a producer's `output_text` bytes map onto grid cells.
 *
 * A producer-constant property, reported **once** through
 * @ref AxlConsoleOps::set_cell_rule immediately after binding. It never varies
 * within a producer, which is why it is not a per-call parameter.
 *
 * The two producers genuinely disagree, and the consumer must not guess: the tap's
 * source is `EFI_SIMPLE_TEXT_OUTPUT`, which is one-cell-per-character by
 * construction — a double-width CJK codepoint occupied one cell on the firmware
 * console, and re-widening it would desync the grid from what the guest believes it
 * drew. A VT byte stream, by contrast, really does carry wide glyphs.
 */
typedef enum {
    AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT = 0, ///< one cell per codepoint (the tap)
    AXL_CONSOLE_CELLS_WIDTH_RESOLVED    = 1, ///< apply @ref axl_vterm_char_width (axl-vterm)
} AxlConsoleCellRule;

/** @brief Which member of @ref AxlConsoleColor is valid. */
typedef enum {
    AXL_CONSOLE_COLOR_DEFAULT = 0, ///< the terminal's own default fg/bg; @c idx and @c rgb unused
    AXL_CONSOLE_COLOR_INDEXED = 1, ///< @c idx is a 0..255 palette index
    AXL_CONSOLE_COLOR_RGB     = 2, ///< @c r / @c g / @c b are 24-bit truecolor
} AxlConsoleColorKind;

/**
 * @brief A tagged foreground/background colour.
 *
 * The tap only ever produces @ref AXL_CONSOLE_COLOR_INDEXED (UEFI's 16-colour
 * foreground / 8-colour background nibbles) or @ref AXL_CONSOLE_COLOR_DEFAULT.
 * @c axl-vterm produces all three.
 */
typedef struct {
    uint8_t kind;       ///< @ref AxlConsoleColorKind
    uint8_t idx;        ///< palette index, when @c kind == AXL_CONSOLE_COLOR_INDEXED
    uint8_t r;          ///< red,   when @c kind == AXL_CONSOLE_COLOR_RGB
    uint8_t g;          ///< green, when @c kind == AXL_CONSOLE_COLOR_RGB
    uint8_t b;          ///< blue,  when @c kind == AXL_CONSOLE_COLOR_RGB
} AxlConsoleColor;

/** @brief Underline style. Mirrors SGR 4 / 21 / 24 and libvterm's 2-bit field. */
typedef enum {
    AXL_CONSOLE_UNDERLINE_OFF    = 0,
    AXL_CONSOLE_UNDERLINE_SINGLE = 1,
    AXL_CONSOLE_UNDERLINE_DOUBLE = 2,
    AXL_CONSOLE_UNDERLINE_CURLY  = 3,
} AxlConsoleUnderline;

/**
 * @brief The full graphic rendition, as a SNAPSHOT.
 *
 * Reported through @ref AxlConsoleOps::set_pen; it latches, and every subsequent
 * @ref AxlConsoleOps::output_text run uses it until the next `set_pen`.
 *
 * **Why a snapshot, and not libvterm's incremental `initpen`/`setpenattr`:** an
 * incremental pen is unimplementable by the tap. The tap observes
 * `SetAttribute(attr)`, which is *already* a whole-pen snapshot; it has no deltas
 * to emit and would have to synthesize them from a diff it does not track. The
 * `axl-vterm` adapter therefore accumulates libvterm's incremental attributes once,
 * so that every consumer does not. This makes the contract honestly **"Layer 2.5"** —
 * a deliberate, minimal slice of libvterm's Layer 3.
 */
typedef struct {
    AxlConsoleColor fg;             ///< foreground colour
    AxlConsoleColor bg;             ///< background colour
    uint8_t         underline;      ///< @ref AxlConsoleUnderline
    bool            bold;
    bool            italic;
    bool            blink;
    bool            reverse;
    bool            conceal;
    bool            strike;
} AxlConsolePen;

/**
 * @brief A half-open cell rectangle: rows `[start_row, end_row)`, cols
 *     `[start_col, end_col)`. 0-based, end-exclusive — the same convention as
 *     libvterm's `VTermRect`, so the adapter is a field-for-field copy.
 *
 * Signed, because scroll deltas are signed and a rect participates in the same
 * arithmetic.
 */
typedef struct {
    int32_t start_row;
    int32_t end_row;
    int32_t start_col;
    int32_t end_col;
} AxlConsoleRect;

/**
 * @brief Extensible terminal properties, reported via @ref AxlConsoleOps::set_term_prop.
 *
 * This one tagged channel **replaces** the former `enable_cursor` and `alt_screen`
 * ops. Note that DECCKM, DECOM, and DECAWM are deliberately absent: they are
 * internal state-machine modes and are never surfaced as properties.
 */
typedef enum {
    AXL_CONSOLE_PROP_CURSOR_VISIBLE = 1, ///< bool
    AXL_CONSOLE_PROP_CURSOR_BLINK   = 2, ///< bool
    AXL_CONSOLE_PROP_ALT_SCREEN     = 3, ///< bool
    AXL_CONSOLE_PROP_TITLE          = 4, ///< string (may arrive in fragments)
    AXL_CONSOLE_PROP_ICON_NAME      = 5, ///< string (may arrive in fragments)
    AXL_CONSOLE_PROP_REVERSE        = 6, ///< bool  (DECSCNM, whole-screen reverse video)
    AXL_CONSOLE_PROP_CURSOR_SHAPE   = 7, ///< number: 1=block, 2=underline, 3=bar
    AXL_CONSOLE_PROP_MOUSE          = 8, ///< number: 0=none, 1=click, 2=drag, 3=move
    AXL_CONSOLE_PROP_FOCUS_REPORT   = 9, ///< bool
} AxlConsoleProp;

/** @brief Which member of @ref AxlConsoleValue is valid. */
typedef enum {
    AXL_CONSOLE_VALUE_BOOL   = 1,
    AXL_CONSOLE_VALUE_NUMBER = 2,
    AXL_CONSOLE_VALUE_STRING = 3,
} AxlConsoleValueKind;

/**
 * @brief The value carried by @ref AxlConsoleOps::set_term_prop.
 *
 * String properties may arrive in **fragments**: @c initial marks the first chunk
 * and @c final the last, so a consumer that cares about titles accumulates across
 * calls. A consumer that does not care simply ignores the string props. The bytes
 * are not NUL-terminated and are only valid for the duration of the call.
 */
typedef struct {
    uint8_t kind;                   ///< @ref AxlConsoleValueKind
    union {
        bool boolean;               ///< @c kind == AXL_CONSOLE_VALUE_BOOL
        int32_t number;             ///< @c kind == AXL_CONSOLE_VALUE_NUMBER
        struct {
            const char *str;        ///< UTF-8 bytes, NOT NUL-terminated
            size_t      len;        ///< byte count
            bool        initial;    ///< first fragment of this string
            bool        final;      ///< last fragment of this string
        } string;                   ///< @c kind == AXL_CONSOLE_VALUE_STRING
    } u;
} AxlConsoleValue;

/**
 * @brief Structured console operations reported to the consumer.
 *
 * **A two-producer contract.** `axl-console-tap` (over UEFI
 * `EFI_SIMPLE_TEXT_OUTPUT`) and `axl-vterm` (over a real VT byte stream) both push
 * these same ops. That constraint shapes the whole vtable: the tap has no cell grid
 * and never can — it only observes `OutputString` / `SetCursorPosition` calls — so a
 * damage-rect *pull* contract (libvterm's Layer 3) is one the tap could not
 * implement. A push op-stream is the only shape both producers speak.
 *
 * Every callback receives the `user` pointer passed at bind time. **All are optional
 * (NULL = ignore that op)**, with one exception documented on @c scrollrect below.
 * They are called from inside the producer's write path, so keep them cheap and
 * non-blocking, and do not re-enter the producer from them.
 *
 * Coordinates are **0-based**; rects are **half-open** (end-exclusive).
 *
 * @par Return values
 *
 * Most ops return `void`. Exactly two return `int`, and in both cases the return is
 * load-bearing rather than decorative — see each one's docs. Resist the temptation
 * to make the vtable uniform: a `int` that every consumer fills with `return 1` is
 * worse than `void`, because a reviewer can no longer tell the meaningful returns
 * from the noise.
 *
 * @par output_text and cell boundaries
 *
 * `output_text` is a cursor-relative UTF-8 **run**, not a per-glyph callback. How
 * its bytes map onto cells is decided by the producer-constant
 * @ref AxlConsoleCellRule reported through @c set_cell_rule. Under
 * @ref AXL_CONSOLE_CELLS_WIDTH_RESOLVED the consumer must apply
 * @ref axl_vterm_char_width to each codepoint, and **must merge zero-width
 * codepoints (combining marks) into the preceding cell** — libvterm packs a base
 * character and its combining sequence into a single cell, and a naive
 * `width >= 1` split would give the mark a cell of its own.
 *
 * From the tap, `output_text` carries UTF-8 decoded from the console's UCS-2,
 * **sanitized** the way the firmware sanitizes it: EDK2's `TerminalConOut.c` accepts
 * only printable `0x20..0x7F` plus `{NUL, BS, TAB, LF, CR}` and substitutes `'?'`
 * for everything else. A UEFI application therefore cannot push raw VT escapes
 * through `OutputString`, and neither can it through this op — forwarding `ESC`
 * verbatim would diverge from firmware semantics and hand any app that prints a
 * user-controlled string an escape-injection vector into the consumer's terminal.
 * (Non-ASCII BMP text *is* passed: the console is UCS-2 and this op is UTF-8.)
 */
typedef struct {
    /**
     * @brief Report this producer's cell-boundary rule. Called **once**, at bind.
     *
     * A consumer that leaves this NULL must assume
     * @ref AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT.
     */
    void (*set_cell_rule)(void *user, AxlConsoleCellRule rule);

    /** @brief Screen cleared to the current pen's background; cursor home. */
    void (*clear_screen)(void *user);

    /** @brief Absolute cursor move (0-based). */
    void (*set_cursor)(void *user, int32_t row, int32_t col);

    /**
     * @brief A cursor-relative run of UTF-8 text (NOT NUL-terminated).
     *
     * Drawn with the pen most recently latched by @c set_pen. Advances the cursor.
     * See the cell-boundary discussion above.
     */
    void (*output_text)(void *user, const char *utf8, size_t len);

    /** @brief Latch the graphic rendition. @c pen is only valid for this call. */
    void (*set_pen)(void *user, const AxlConsolePen *pen);

    /** @brief Text mode changed (UEFI mode number). Tap-only; `axl-vterm` never calls it. */
    void (*set_mode)(void *user, uint32_t mode);

    /**
     * @brief Erase a rectangle to the current pen's background.
     *
     * @c selective means DECSEL/DECSED: leave DECSCA-protected cells alone.
     *
     * **Must be bound if @c scrollrect is bound**, because a declined @c scrollrect
     * decomposes into @c moverect + @c erase and the decomposition calls @c erase
     * unconditionally (libvterm `vterm.c:383` has no NULL guard, unlike its
     * @c moverect call at `vterm.c:369`).
     */
    void (*erase)(void *user, AxlConsoleRect rect, bool selective);

    /**
     * @brief Blit @c src onto @c dest. Both rects have identical dimensions.
     *
     * **Argument order is `(dest, src)`.** libvterm's `vterm_scroll_rect()`
     * *prototype* names its callback parameters `(src, dest)`, but its body calls
     * `(*moverect)(dest, src, user)` (`vterm.c:370`), matching
     * `VTermStateCallbacks.moverect` and `screen.c`'s own implementation. The
     * prototype's names are an upstream cosmetic bug. Getting this backwards blits
     * the wrong direction.
     *
     * The tap never calls this. Optional (NULL ⇒ the caller falls back to erase).
     */
    void (*moverect)(void *user, AxlConsoleRect dest, AxlConsoleRect src);

    /** @brief BEL. */
    void (*bell)(void *user);

    /**
     * @brief Scroll @c rect by @c downward rows and @c rightward cols.
     *
     * Positive @c downward scrolls content **up** (a new blank row appears at the
     * bottom); positive @c rightward scrolls content **left**.
     *
     * @return non-zero if the consumer handled it (e.g. by a GOP blit), 0 to
     *     **decline**. This return is load-bearing: on a decline, `axl-vterm` calls
     *     libvterm's `vterm_scroll_rect()`, which decomposes the scroll into
     *     @c moverect + @c erase. A consumer that cannot blit returns 0 and gets
     *     ordinary damage. Widen this to `void` and that fallback silently dies.
     *
     * The tap never calls this. NULL is equivalent to always declining, except that
     * `axl-vterm` then skips straight to the decomposition.
     */
    int (*scrollrect)(void *user, AxlConsoleRect rect, int32_t downward, int32_t rightward);

    /**
     * @brief Set a terminal property. Replaces the former `enable_cursor`/`alt_screen`.
     *
     * @return non-zero if the consumer accepted the property, 0 to reject it. This
     *     return is load-bearing and is the one most easily got wrong: libvterm only
     *     stores the new value of a property **if the callback said it was happy**
     *     (`state.c:2223`, *"This is especially important for altscreen switching"*).
     *     Return 0 from `AXL_CONSOLE_PROP_ALT_SCREEN` and libvterm's internal
     *     alt-screen state never flips, desyncing the parser from the grid. Accept
     *     (return 1) any property you act on, and any property you deliberately
     *     ignore.
     *
     * @c val is only valid for the duration of the call.
     */
    int (*set_term_prop)(void *user, AxlConsoleProp prop, const AxlConsoleValue *val);

    /**
     * @brief Clear the consumer's scrollback history (xterm `CSI 3J`).
     *
     * Layer 2's *only* scrollback callback. A VT stream expresses "erase the
     * saved-lines buffer" as `CSI 3J`; libvterm surfaces it as `sb_clear`, which
     * `axl-vterm` maps here. `axl-console-tap` never calls this. Optional
     * (NULL ⇒ `CSI 3J` is a no-op). The visible screen is unaffected — only the
     * off-screen history the consumer owns (libvterm keeps none itself).
     */
    void (*clear_scrollback)(void *user);
} AxlConsoleOps;

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_OPS_H */
