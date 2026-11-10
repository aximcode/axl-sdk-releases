/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file sshd-bind.h
 * @brief The pure half of the `sshd` tool's console binding.
 *
 * Everything here is arithmetic on bytes and a small table: no UEFI, no
 * I/O, no allocation, no clock of its own (every entry point is handed
 * "now" in milliseconds).  That is what lets it be unit-tested exactly.
 *
 * Three pieces:
 *
 *  - an input reassembler (SshdInput), which turns the arbitrary packet
 *    boundaries of an SSH channel into whole keystrokes for
 *    axl_console_input_inject_text(), which treats every call as
 *    self-contained and so mis-reads an escape sequence or a UTF-8
 *    character cut in two;
 *  - the session table (SshdSessions), the set of shell channels that are
 *    watching the console, each either live or BEHIND;
 *  - the rule for when a late joiner or a recovered session may be handed
 *    a repaint (sshd_bind_snapshot_allowed()), the repaint's own progress,
 *    and how one flush of console output reaches one session
 *    (sshd_fan_write());
 *  - a per-session pending-input queue (SshdPend) that holds what the
 *    console's 63-key ring cannot take yet, in order and in whole runs.
 */

#ifndef AXL_TOOLS_SSHD_BIND_H
#define AXL_TOOLS_SSHD_BIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** How long an incomplete trailing escape sequence or UTF-8 character is
 *  held waiting for its remainder before it is given up on.
 *
 *  The cost is paid by exactly one key: a bare Esc, which cannot be told
 *  from the start of a sequence and so reaches the Shell this much late.
 *  The benefit is a sequence split across two packets (an MTU boundary, a
 *  coalescing middlebox) being read as the arrow key it is, not as Esc,
 *  '[' and 'A'.
 *
 *  100 ms, from how other software that faces the same ambiguity is tuned:
 *  readline waits 500 ms (keyseq-timeout), tmux defaults to 500 ms
 *  (escape-time) and is routinely cut to 10-50 ms, vim's defaults.vim uses
 *  100 ms (ttimeoutlen), neovim 50 ms, fish 30 ms, zsh 400 ms (KEYTIMEOUT).
 *  Those are tuned for serial lines and slow links, where the bytes of one
 *  sequence really can be far apart.  Here an SSH client writes a whole
 *  sequence in one channel-data packet, so a split is the rare case and
 *  the gap inside it is network jitter, not a typing delay; 100 ms
 *  covers a slow WAN's jitter while a bare Esc stays under the roughly
 *  100 ms a person perceives as instant. */
#define SSHD_ESC_TIMEOUT_MS    100

/** Longest escape sequence the reassembler will hold: room for the longest
 *  key a terminal sends, modifyOtherKeys' ESC [ 27 ; 5 ; 9 ~ (10 bytes) and
 *  CSI-u's ESC [ 127 ; 5 u, with some to spare.  A longer one is malformed
 *  and is dropped. */
#define SSHD_INPUT_PEND_MAX    16

/** Most bytes of an overlong escape sequence that are swallowed before the
 *  reassembler gives up skipping and treats what follows as ordinary input,
 *  so a stream of parameter bytes cannot silence a session indefinitely. */
#define SSHD_DISCARD_MAX       64

/** Most shell sessions the table holds at once. */
#define SSHD_SESSIONS_MAX      16

/**
 * Per-session input reassembly state.  Zero-initialise before first use
 * (`SshdInput in = {0};`); there is no init function and nothing to free.
 */
typedef struct {
    uint8_t  pend[SSHD_INPUT_PEND_MAX]; ///< held bytes: an unfinished escape or UTF-8 sequence
    size_t   npend;                     ///< bytes in pend[]
    uint64_t pend_since_ms;             ///< when the last byte joined pend[] / was skipped, or the last CR was seen
    bool     last_cr;                   ///< the previous byte was CR, so an LF now is its CRLF tail
    bool     discarding;                ///< an overlong sequence is being skipped up to its final byte
    uint8_t  ndiscard;                  ///< bytes skipped so far in this overlong sequence
} SshdInput;

/** What an emitted run is. */
typedef enum {
    SSHD_IN_TEXT,   ///< bytes for axl_console_input_inject_text(): whole sequences only
    SSHD_IN_CTRL    ///< one control key; bytes[0] is the lower-case letter 'a'..'z'
} SshdInKind;

/**
 * Receives each run of reassembled input, in input order.
 *
 * @param kind   ///< SSHD_IN_TEXT or SSHD_IN_CTRL
 * @param bytes  ///< the run; NOT NUL-terminated, valid only during the call
 * @param len    ///< its length: >= 1; exactly 1 for SSHD_IN_CTRL
 * @param ud     ///< the pointer given to sshd_input_feed() / sshd_input_tick()
 *
 * A TEXT run is never split inside an escape sequence, an Alt chord or a
 * UTF-8 character, so each may be handed to inject_text() independently.
 * A CTRL run is meant for axl_console_input_inject_key_ex(0, letter, ...)
 * with the left-Control shift state (plus VALID) and the toggle state 0xC0
 * (AXL_CONSOLE_TOGGLE_KEYBOARD) -- the key inject_text() itself would make
 * of the control byte, as a USB keyboard reports it; it is a run of its own
 * only so the caller can order it (Ctrl-S) or let it jump the queue
 * (Ctrl-C).
 */
typedef void (*SshdInEmit)(SshdInKind kind, const char *bytes, size_t len, void *ud);

/**
 * @brief Feed one packet of channel data through the reassembler.
 *
 * Applies, in order of precedence per byte:
 *  - CR becomes CR; LF becomes CR; an LF directly after a CR (even in an
 *    earlier packet) is swallowed, so CRLF is one Enter (the Shell takes
 *    only CR as Enter).  A CRLF whose halves arrive more than
 *    SSHD_ESC_TIMEOUT_MS apart is two Enters;
 *  - 0x01..0x1A other than BS (0x08), TAB (0x09), LF and CR are control
 *    keys, emitted as SSHD_IN_CTRL with the letter 'a'..'z' (0x03 gives
 *    'c', 0x13 gives 's'); BS and TAB stay text, being keys the Shell
 *    already understands, and so do NUL (Ctrl-Space) and 0x1C..0x1F, which
 *    the decoder turns into their Ctrl chords;
 *  - ESC starts a sequence: ESC [ params final, or ESC O final, is held
 *    until complete and emitted whole.  One that outgrows
 *    SSHD_INPUT_PEND_MAX is dropped through its final byte, the skip ending
 *    early at a control byte, a byte >= 0x7f, SSHD_ESC_TIMEOUT_MS of
 *    silence or SSHD_DISCARD_MAX bytes.  A sequence broken off by a control
 *    or non-ASCII byte is emitted as the keys it was (the same as when it
 *    times out), and the byte is then processed afresh.  Its bytes end the
 *    run they join -- text earlier in the same packet coalesces with them
 *    -- so nothing typed after the break is glued onto the sequence.
 *    An Alt chord is ESC and its key IN THE SAME PACKET, as a terminal
 *    sends it: ESC and a control key (one of the bytes above that become
 *    SSHD_IN_CTRL) is one Alt chord, emitted whole as text; ESC ESC ending
 *    a packet is one run (Alt+Esc); ESC and any other byte ends the hold,
 *    and that byte is processed afresh in the same run (an LF still
 *    becoming CR), where the decoder reads ESC and that key as one Alt
 *    chord.  An ESC held from an earlier packet is a bare Esc, a run of its
 *    own, whatever the next packet starts with -- Esc then Ctrl-C is still
 *    the break, Esc then x is two keys -- except '[' or 'O', which continue
 *    a sequence split across packets;
 *  - a UTF-8 lead byte is held until its continuation bytes arrive.
 *    Only well-formed UTF-8 is passed on: overlong forms, surrogates and
 *    code points above U+10FFFF are dropped here, because the console
 *    input decoder does not validate and would turn them into bogus keys.
 *    A stray continuation byte is dropped too;
 *  - everything else is text.
 *
 * Adjacent text is coalesced into one run.  Anything held for
 * SSHD_ESC_TIMEOUT_MS or more is resolved first, exactly as
 * sshd_input_tick() would have, and as a run of its own: a stale ESC must
 * not be glued to a following '[' and read back as a sequence.
 *
 * @param in      ///< session state; must not be NULL
 * @param buf     ///< the packet bytes; may be NULL only when len is 0
 * @param len     ///< packet length
 * @param now_ms  ///< a monotonic millisecond clock
 * @param emit    ///< run sink; must not be NULL
 * @param ud      ///< passed through to emit
 */
void sshd_input_feed(
    SshdInput     *in,
    const uint8_t *buf,
    size_t         len,
    uint64_t       now_ms,
    SshdInEmit     emit,
    void          *ud);

/**
 * @brief Resolve held input that has waited long enough.
 *
 * Call from a periodic tick.  When something has been held for
 * SSHD_ESC_TIMEOUT_MS or more, a held escape prefix (a lone ESC, or ESC
 * with its introducer and no final byte) is emitted as text, as the keys
 * it would have been without reassembly; a held partial UTF-8 character
 * is dropped.  The "previous byte was CR" memory expires on the same
 * timeout, so an LF typed seconds after an Enter is a second Enter.  A
 * clock that has gone backwards counts as expired.
 *
 * @param in      ///< session state; must not be NULL
 * @param now_ms  ///< a monotonic millisecond clock
 * @param emit    ///< run sink; must not be NULL
 * @param ud      ///< passed through to emit
 */
void sshd_input_tick(
    SshdInput *in,
    uint64_t   now_ms,
    SshdInEmit emit,
    void      *ud);

/** One shell channel watching the console. */
typedef struct {
    void      *chan;    ///< the caller's channel handle; the lookup key, never dereferenced
    SshdInput  in;      ///< this session's input reassembly state
    bool       behind;  ///< fell behind on output: skipped by the fan-out until repainted
    bool       used;    ///< slot is occupied (table internals)
    uint32_t   seq;     ///< admission order (table internals)
} SshdSession;

/**
 * The session table.  Zero-initialise before first use
 * (`SshdSessions t = {0};`); there is nothing to free.  A removal leaves
 * the slot reusable and never moves another session, so a pointer to a
 * live session stays valid until that session is removed.
 */
typedef struct {
    SshdSession slots[SSHD_SESSIONS_MAX];  ///< storage
    uint32_t    next_seq;                  ///< admission counter (table internals)
} SshdSessions;

/** Which sessions sshd_sessions_each() visits. */
typedef enum {
    SSHD_SESS_LIVE,     ///< not BEHIND: the ones a fan-out writes to
    SSHD_SESS_BEHIND,   ///< BEHIND: the ones owed a repaint
    SSHD_SESS_ANY       ///< all of them
} SshdSessSel;

/** Per-session callback for sshd_sessions_each().
 *  May call sshd_sessions_remove() on any session, including the one it
 *  was handed.  Must not use that pointer after removing it. */
typedef void (*SshdSessFn)(SshdSession *s, void *ud);

/**
 * @brief Admit a session.
 *
 * The new session starts BEHIND: it owes a snapshot before it may
 * receive live output, which is how a late join and a recovery are the
 * same path.  Its input state starts zeroed.
 *
 * @param t     ///< the table
 * @param chan  ///< the caller's channel handle; must not be NULL
 * @return the session, or NULL when chan is NULL, already present, or
 *         the table is full (SSHD_SESSIONS_MAX)
 */
SshdSession *sshd_sessions_add(SshdSessions *t, void *chan);

/**
 * @brief Remove a session.
 *
 * Safe from inside an sshd_sessions_each() callback; the session is
 * not visited afterwards if it had not been yet.
 *
 * @param t     ///< the table
 * @param chan  ///< the handle given to sshd_sessions_add()
 * @return true if a session was removed, false if chan is not present
 */
bool sshd_sessions_remove(SshdSessions *t, void *chan);

/**
 * @brief Look a session up by its channel handle.
 *
 * @param t     ///< the table
 * @param chan  ///< the handle given to sshd_sessions_add()
 * @return the session, or NULL
 */
SshdSession *sshd_sessions_find(SshdSessions *t, void *chan);

/**
 * @brief Mark a session BEHIND: skipped by SSHD_SESS_LIVE until cleared.
 *
 * @param t     ///< the table
 * @param chan  ///< the handle given to sshd_sessions_add()
 * @return true if the session exists
 */
bool sshd_sessions_mark_behind(SshdSessions *t, void *chan);

/**
 * @brief Clear BEHIND: the session is live again.
 *
 * @param t     ///< the table
 * @param chan  ///< the handle given to sshd_sessions_add()
 * @return true if the session exists
 */
bool sshd_sessions_clear_behind(SshdSessions *t, void *chan);

/**
 * @brief Visit the selected sessions in admission order.
 *
 * Removal during the walk is safe.  A session added during the walk is
 * not visited by it.  A session that changes state during the walk is
 * judged by its state when its turn comes.
 *
 * @param t    ///< the table
 * @param sel  ///< which sessions
 * @param fn   ///< called once per visited session
 * @param ud   ///< passed through to fn
 */
void sshd_sessions_each(
    SshdSessions *t,
    SshdSessSel   sel,
    SshdSessFn    fn,
    void         *ud);

/**
 * @brief How many sessions the table holds.
 *
 * @param t  ///< the table
 * @return the count, BEHIND ones included
 */
size_t sshd_sessions_count(const SshdSessions *t);

/**
 * @brief May a join or repaint snapshot be taken now?
 *
 * Snapshotting the screen model while a console operation is part-way
 * through changing it would capture a torn screen, so the caller counts
 * operations in flight and defers the snapshot to its next tick while any
 * are.
 *
 * @param in_op  ///< operations currently in flight on the console binding
 * @return true when in_op is 0
 */
bool sshd_bind_snapshot_allowed(unsigned in_op);

/**
 * A session's repaint in progress: the bookkeeping only.  The bytes are the
 * caller's (a snapshot of the screen model), handed back in on every
 * sshd_repaint_drain() call.  Zero-initialise; nothing to free.
 */
typedef struct {
    size_t   len;     ///< repaint length
    size_t   off;     ///< bytes already accepted by the channel
    uint64_t gen;     ///< the console's op generation when it was taken
    bool     active;  ///< a repaint is being written
} SshdRepaint;

/** What a drain step decided. */
typedef enum {
    SSHD_RP_REJOIN,    ///< all written, and nothing changed since the snapshot:
                       ///< the session may rejoin the live stream
    SSHD_RP_RETAKE,    ///< all written, but the console changed meanwhile: the
                       ///< repaint is stale, so take a fresh one
    SSHD_RP_BLOCKED,   ///< the channel refused part-way; resume on writable
    SSHD_RP_FAILED     ///< the channel refused for good, or is already closed
} SshdRpResult;

/** What one channel write did: all of it queued, none (try again after the
    channel's writable), none because the channel is already closed, or none
    ever for any other reason (the channel must be closed). */
typedef enum {
    SSHD_WR_OK,
    SSHD_WR_BUSY,
    SSHD_WR_FAILED,
    SSHD_WR_GONE
} SshdWrResult;

/**
 * @brief Must a channel whose write was refused be closed by the caller?
 * Only a refusal for good that left the channel open: a BUSY channel owes
 * a writable and recovers, a GONE one is already closed (its close hook is
 * coming), and leaving a FAILED one open would leave the session skipped
 * for ever with its client still connected.
 * @param r  ///< the refused write's result
 * @return true for SSHD_WR_FAILED only
 */
bool sshd_bind_close_on(SshdWrResult r);

/** What giving the console back came to, once the Shell has exited. */
typedef enum {
    SSHD_REL_RETRY,     ///< a call into the console is in flight: nothing changed, try next tick
    SSHD_REL_RELEASED,  ///< the firmware has its console back
    SSHD_REL_STUCK      ///< the firmware would not let go: the console is still taken
} SshdRelease;

/**
 * @brief Read the console device's uninstall status as a release outcome.
 * @param uninstall_rc  ///< what axl_console_device_uninstall() returned
 * @return SSHD_REL_RETRY for AXL_BUSY, SSHD_REL_RELEASED for AXL_OK, and
 *         SSHD_REL_STUCK for anything else -- never "released" when it was not
 */
SshdRelease sshd_bind_release_of(int uninstall_rc);

/**
 * @brief The line every session is sent when the Shell has exited, saying
 *        what became of the console.
 * @param r  ///< SSHD_REL_RELEASED or SSHD_REL_STUCK
 * @return a static line, unframed (sshd_bind_goodbye_text() frames it);
 *         NULL for SSHD_REL_RETRY (nothing to say yet)
 */
const char *sshd_bind_goodbye(SshdRelease r);

/** The line every session is sent by `sshd stop` (or an unload). */
#define SSHD_STOPPED_TEXT "[sshd] stopped"

/**
 * @brief Assemble the last bytes a session is sent: a goodbye line.
 *
 * The line goes on a line of its own (CRLF before and after) and erases to
 * its end (ESC [ K), so no text the console left on that row trails it. A
 * session whose larger terminal is confined to a scroll region first has the
 * region reset (ESC [ r) so its user's own shell gets the whole terminal
 * back -- between DECSC and DECRC (ESC 7, ESC 8), because DECSTBM homes the
 * cursor and the line would otherwise land on row 2 over the console's
 * image instead of below it.
 *
 * @param buf     ///< receives the bytes, NUL-terminated
 * @param cap     ///< size of buf, in bytes
 * @param region  ///< the session's terminal is confined to a scroll region
 * @param text    ///< the line, unframed: sshd_bind_goodbye() or SSHD_STOPPED_TEXT
 * @return the length written (without the NUL); 0, with buf set to "", if
 *         it does not fit -- never a sequence cut short
 */
size_t sshd_bind_goodbye_text(char *buf, size_t cap, bool region, const char *text);

/** What the driver holds of the console take-over. */
typedef enum {
    SSHD_CON_NONE,    ///< nothing: never taken, or given back
    SSHD_CON_TAKEN,   ///< taken, serving
    SSHD_CON_STUCK    ///< an uninstall failed for good: the device is still
                      ///< live, calling into this image and its encoder
} SshdCon;

/**
 * @brief Apply one uninstall attempt to the take-over's state.
 *
 * A busy device changes nothing.  AXL_OK gives the console back: the state
 * becomes SSHD_CON_NONE, and only then may the caller drop the device
 * handle and free the encoder.  Anything else is SSHD_CON_STUCK, and the
 * handle and the encoder must both be KEPT: the device still calls into
 * them, and a later attempt on the same handle resumes where this one
 * stopped.  Dropping the handle on a failure would make that later attempt
 * an uninstall of NULL -- which reports AXL_OK -- and the image would then
 * be unloaded under a live take-over.  Call only while the state is not
 * SSHD_CON_NONE: there is nothing to uninstall then.
 *
 * @param c   ///< [in,out] the state
 * @param rc  ///< what axl_console_device_uninstall() returned
 * @return the attempt's outcome, as sshd_bind_release_of() reads @a rc
 */
SshdRelease sshd_bind_con_drop(SshdCon *c, int rc);

/** What a setup that failed after taking the console must do. */
typedef enum {
    SSHD_SETUP_FAIL,   ///< the console is back: setup may return an error and the image unload
    SSHD_SETUP_STAY    ///< the take-over is still in place: the image must stay resident
} SshdSetupFail;

/**
 * @brief May a setup that failed, having taken the console, let its image go?
 *
 * Returning an error from setup unloads the image, and a take-over the
 * firmware would not release (or could not release yet, the device being
 * mid-call) still calls into it.  So only a released console lets setup
 * fail.  Otherwise the image stays resident, publishes that it is stuck, and
 * `sshd stop` retries the release, as it does after the Shell has exited.
 *
 * @param r  ///< what releasing the console came to (sshd_bind_release_of())
 * @return SSHD_SETUP_FAIL for SSHD_REL_RELEASED only
 */
SshdSetupFail sshd_bind_setup_failed(SshdRelease r);

/// Why a driver whose console take-over could not be undone is not serving:
/// the reason it gives axl_service_set_degraded, and the tail of the line
/// `sshd status` prints for it, stopping or degraded alike.
#define SSHD_STUCK_REASON \
    "the console take-over could not be undone; 'sshd stop' tries again"

/**
 * @brief What `sshd status` reports, after "sshd: ".
 * @param loaded  ///< the driver image is resident
 * @param taken   ///< a console take-over device is present
 * @param stuck   ///< the driver has published that its release is stuck
 * @return a static string: "stopped", "running", the exited-Shell line, or
 *         -- whenever @a stuck, whatever else holds -- the stuck line, which
 *         is never a plain "running"
 */
const char *sshd_bind_status(bool loaded, bool taken, bool stuck);

/**
 * Write one piece, all or nothing.
 *
 * @param bytes  ///< the piece; valid only during the call
 * @param len    ///< its length, >= 1
 * @param ud     ///< passed through from sshd_repaint_drain()
 */
typedef SshdWrResult (*SshdRpWrite)(const char *bytes, size_t len, void *ud);

/**
 * @brief Start a repaint of @a len bytes taken at op generation @a gen.
 *
 * @param r    ///< the session's repaint state
 * @param len  ///< length of the snapshot the caller now holds
 * @param gen  ///< the console's op generation at the snapshot
 */
void sshd_repaint_begin(SshdRepaint *r, size_t len, uint64_t gen);

/**
 * @brief Write what is left of a repaint, and judge it once it is all out.
 *
 * Writes @a bytes from where the last call stopped, in pieces of at most
 * @a piece bytes, until all of it is accepted or a write is refused.  A
 * repaint refused part-way is RESUMED by the next call rather than started
 * over: one longer than the channel's queue could otherwise never finish.
 *
 * Resuming has a cost the verdict pays: the console kept changing while the
 * session was stalled, and the bytes being finished describe the screen as
 * it WAS.  So a repaint that is all out is judged against @a now_gen: equal
 * to its snapshot's generation, the session's screen is current
 * (SSHD_RP_REJOIN); different, it is stale and the caller must take a fresh
 * snapshot and begin again (SSHD_RP_RETAKE).  Either way the repaint is no
 * longer active.  So is it after SSHD_RP_FAILED.
 *
 * @param r        ///< state from sshd_repaint_begin(); must be active
 * @param bytes    ///< the snapshot, @a r->len bytes, the same every call
 * @param now_gen  ///< the console's op generation now
 * @param piece    ///< largest single write, >= 1
 * @param write    ///< the channel writer
 * @param ud       ///< passed through to write
 * @return the verdict; SSHD_RP_FAILED also for an inactive @a r
 */
SshdRpResult sshd_repaint_drain(
    SshdRepaint *r,
    const char  *bytes,
    uint64_t     now_gen,
    size_t       piece,
    SshdRpWrite  write,
    void        *ud);

/** What one flush did on one session. */
typedef enum {
    SSHD_FAN_SENT,      ///< queued whole, in one write
    SSHD_FAN_BEHIND,    ///< refused BUSY, nothing queued: the session is BEHIND
                        ///< and waits for its writable
    SSHD_FAN_TOO_BIG,   ///< longer than the channel limit, nothing written: no
                        ///< session can take it, so every one is repainted
    SSHD_FAN_FAILED,    ///< refused for good, nothing queued (close it)
    SSHD_FAN_GONE       ///< the channel is already closed
} SshdFanResult;

/**
 * @brief Hand one flush of console output to one session, whole or not at
 *     all.
 * A flush ends between console operations, so it is a sequence of whole
 * escape sequences and characters; the session's stream stays well-formed
 * only if each flush reaches it entirely or not at all.  So this makes ONE
 * write of all @a len bytes, or none: a flush longer than @a limit (the
 * channel's per-channel queue limit, which a single write may never exceed)
 * is not attempted, and a refused write queued nothing.  A session that
 * misses a flush is BEHIND and is repainted from the screen model, which
 * starts with a clear, so what it missed never shows as a torn sequence.
 * @param bytes  ///< the flush; may be NULL only when len is 0
 * @param len    ///< its length; 0 writes nothing and is SSHD_FAN_SENT
 * @param limit  ///< the most one write may carry
 * @param write  ///< the channel writer, all-or-nothing per call
 * @param ud     ///< passed through to write
 * @return the outcome
 */
SshdFanResult sshd_fan_write(
    const char  *bytes,
    size_t       len,
    size_t       limit,
    SshdRpWrite  write,
    void        *ud);

/** Bytes of pending input one session may hold: a long paste that the
 *  console's key ring cannot take at once waits here.  Generous on purpose
 *  -- an operator's paste of a script must not drop -- and per session, so
 *  one client cannot spend another's. */
#define SSHD_PEND_BYTES    (64u * 1024u)

/**
 * A session's pending input: runs from the reassembler, kept whole and in
 * order, CTRL keys included, until the console takes them.  The record
 * framing keeps each run's boundary, because the console decoder treats
 * each call as self-contained: two runs glued into one call could read a
 * timed-out escape prefix and the next run's first byte as one sequence.
 * Zero-initialise.  The SSHD_PEND_BYTES buffer is allocated by the first
 * push, so a session that never has to wait costs only this header; free it
 * with sshd_pend_free().
 */
typedef struct {
    uint8_t *buf;                  ///< records: kind (1), length (2, LE), bytes;
                                   ///< NULL until the first push
    size_t   head;                 ///< offset of the oldest record
    size_t   used;                 ///< bytes of records from head
    uint64_t dropped;              ///< input bytes refused because they did not fit
} SshdPend;

/**
 * Takes the front of one pending run.
 * @param kind   ///< SSHD_IN_TEXT or SSHD_IN_CTRL (len 1, the letter)
 * @param bytes  ///< what is left of the run; valid only during the call
 * @param len    ///< its length, >= 1
 * @param ud     ///< passed through from sshd_pend_drain()
 * @return how many bytes it took, 0..len.  Less than len stops the drain
 *         and keeps the rest, which is offered again by the next drain --
 *         so a TEXT taker must stop only between whole characters and
 *         sequences, and a CTRL taker takes all or nothing.
 */
typedef size_t (*SshdPendTake)(SshdInKind kind, const char *bytes, size_t len, void *ud);

/**
 * @brief Queue a run behind whatever is pending.
 * All or nothing: a run that does not fit whole is refused, counted in
 * @a p->dropped, and none of it is queued -- a run cut short could end
 * inside an escape sequence.  The first push allocates the buffer; when
 * that fails the run is refused and counted the same way.
 * @param p     ///< the queue
 * @param kind  ///< the run's kind
 * @param bytes ///< the run; copied
 * @param len   ///< its length, 1..65535
 * @return true when queued
 */
bool sshd_pend_push(SshdPend *p, SshdInKind kind, const char *bytes, size_t len);

/**
 * @brief Is nothing pending?  New input must queue behind pending input,
 *     never overtake it.
 * @param p  ///< the queue
 * @return true when empty
 */
bool sshd_pend_empty(const SshdPend *p);

/**
 * @brief Offer the pending runs, oldest first, until one is not taken whole.
 * @param p     ///< the queue
 * @param take  ///< the taker
 * @param ud    ///< passed through to take
 * @return true when the queue is now empty
 */
bool sshd_pend_drain(SshdPend *p, SshdPendTake take, void *ud);

/**
 * @brief Does this control key act at once, ahead of input still waiting?
 *
 * Only Ctrl-C ('c'): the chord the Shell registers its break notify for,
 * acted on at the press as a pty's ISIG characters are.  Every other
 * control key is input like any other and keeps its place, as in a pty:
 * the UEFI editor reads ^S ^Q ^K ^U ^F as commands, and one that overtook a
 * paste would act part-way through it -- a ^Q could quit the editor and
 * type the rest of the paste at the prompt.
 *
 * @param letter  ///< the CTRL run's letter, 'a'..'z'
 * @return true for 'c' only
 */
bool sshd_ctrl_bypasses(char letter);

/**
 * @brief May this control key go into the console's key ring now?
 *
 * A key's notifies fire when it is PUSHED, not when the guest reads it, so
 * a chord pushed behind keys the guest has not read yet acts before them.
 * The Shell's Ctrl-S is the case that bites: its notify halts output, and
 * the next key READ resumes it and is consumed -- one of the keys typed
 * before the Ctrl-S ('echo abcdef' + ^S in one write read as 'eho abcdef').
 * So an ordered control key waits until the guest has read everything
 * ahead of it (@a unread_keys 0), as at a keyboard typed at human speed.
 * A key with no notify loses nothing by waiting: it was going to be read
 * after those keys anyway.  A bypassing key (sshd_ctrl_bypasses()) never
 * waits: Ctrl-C's break must reach a command that is not reading.
 *
 * @param letter       ///< the CTRL run's letter, 'a'..'z'
 * @param unread_keys  ///< keys in the console's ring the guest has not read
 * @return true when the key may be injected now
 */
bool sshd_ctrl_may_enter(char letter, size_t unread_keys);

/**
 * @brief Hand one reassembled run towards the console, in order.
 *
 * A bypassing control key (sshd_ctrl_bypasses()) is offered to @a take at
 * once and never queued, whatever @a take answers: its notify fires on
 * the press even when the key itself is refused, and a second offer would
 * fire it twice.  Anything else -- text, or any other control key -- is
 * offered now only when nothing is pending, and what @a take leaves is
 * queued behind; with input already pending it is queued without an
 * offer, so it never overtakes what waits.
 *
 * @param p     ///< the session's pending input
 * @param kind  ///< the run's kind
 * @param bytes ///< the run
 * @param len   ///< its length, >= 1 (1 for SSHD_IN_CTRL)
 * @param take  ///< the taker, as for sshd_pend_drain()
 * @param ud    ///< passed through to take
 * @return false when what had to be queued did not fit and was dropped
 *         (counted in @a p->dropped)
 */
bool sshd_input_route(SshdPend *p, SshdInKind kind, const char *bytes, size_t len,
                      SshdPendTake take, void *ud);

/**
 * @brief Release the queue's buffer, dropping anything still pending.
 * The queue is then empty and may be used again.  NULL-safe, idempotent.
 * @param p  ///< the queue, or NULL
 */
void sshd_pend_free(SshdPend *p);

/** A one-line-per-period log limiter.  Zero-initialise. */
typedef struct {
    uint64_t last_ms;     ///< when the last line was let through
    uint32_t suppressed;  ///< events held back since then
    bool     ever;        ///< a line has been let through
} SshdRateLimit;

/**
 * @brief Should this event get a log line?
 * The first event always does; after it, at most one per @a period_ms.
 * Events held back are counted, and the count is handed to the next line
 * let through (then reset), so a burst is reported as a number rather than
 * lost.
 * @param r           ///< the limiter
 * @param now_ms      ///< a monotonic millisecond clock
 * @param period_ms   ///< the least time between lines
 * @param suppressed  ///< [out] events held back since the last line, when
 *                    ///< this returns true; may be NULL
 * @return true when the caller should log
 */
bool sshd_ratelimit(SshdRateLimit *r, uint64_t now_ms, uint64_t period_ms,
                    uint32_t *suppressed);

/** How a session's terminal fits the console it shows. */
typedef enum {
    SSHD_FIT_EXACT,    ///< the console's size, or no size given (taken as the console's)
    SSHD_FIT_LARGER,   ///< at least as large both ways and larger one way: confined
    SSHD_FIT_SMALLER   ///< smaller either way: held behind a notice until it grows
} SshdFit;

/**
 * @brief How a terminal of @a term_cols x @a term_rows fits a console of
 * @a con_cols x @a con_rows.
 *
 * The console is never reshaped for a session: other sessions and the local
 * screen show it too. A terminal that gives no size (0 either way: no pty,
 * or `ssh -tt` reading a pipe) is taken to be the console's size.
 *
 * @param con_cols   ///< the console's width
 * @param con_rows   ///< the console's height
 * @param term_cols  ///< the terminal's width, from pty-req or window-change
 * @param term_rows  ///< the terminal's height
 * @return the fit
 */
SshdFit sshd_fit(uint32_t con_cols, uint32_t con_rows, uint32_t term_cols,
                 uint32_t term_rows);

/**
 * @brief What a repaint must start with for a terminal of this fit.
 *
 * A LARGER terminal gets a scroll region of the console's rows (DECSTBM
 * `ESC[1;<rows>r`), so a line feed on the console's last row scrolls those
 * rows and not the whole terminal; the rows below stay blank (the repaint
 * clears them, and nothing is written there). A terminal back at the
 * console's size gets the region reset (`ESC[r`) when one was set. Wider
 * terminals need nothing more: the encoder wraps at the console's width.
 *
 * @param buf         ///< receives the bytes, NUL-terminated
 * @param cap         ///< its size
 * @param fit         ///< the session's fit
 * @param con_rows    ///< the console's height
 * @param region_set  ///< a region was set on this terminal earlier
 * @return the length written; 0 when there is nothing to write or @a cap is
 *         too small for it (then nothing is written)
 */
size_t sshd_fit_prefix(char *buf, size_t cap, SshdFit fit, uint32_t con_rows,
                       bool region_set);

/**
 * @brief The screen a SMALLER terminal is shown instead of the console.
 *
 * Clears the terminal (resetting any scroll region) and says both sizes,
 * what to do, and that input still reaches the console. The session gets no
 * console output until a window-change makes it large enough; then it is
 * repainted.
 *
 * @param buf        ///< receives the bytes, NUL-terminated
 * @param cap        ///< its size
 * @param con_cols   ///< the console's width
 * @param con_rows   ///< the console's height
 * @param term_cols  ///< the terminal's width
 * @param term_rows  ///< the terminal's height
 * @return the length written; 0 when @a cap is too small (nothing written)
 */
size_t sshd_held_notice(char *buf, size_t cap, uint32_t con_cols, uint32_t con_rows,
                        uint32_t term_cols, uint32_t term_rows);

/**
 * @brief Whether anyone could log in: a key with PubkeyAuthentication on,
 *     or a password with PasswordAuthentication on. The one rule the
 *     launcher refuses to start by and the driver checks again.
 *
 * @param pubkey        ///< PubkeyAuthentication
 * @param have_keys     ///< an authorized_keys file is set
 * @param pwauth        ///< PasswordAuthentication
 * @param have_password ///< a password is set
 * @return true when at least one way in is open
 */
bool sshd_bind_can_log_in(bool pubkey, bool have_keys, bool pwauth, bool have_password);

/**
 * @brief Whether the driver's own log is written while it serves, and after.
 *
 * While it serves, sshd's log goes to its serial port (AxlLogPort), never
 * into the sessions; with no port it is not written at all. When it stops, the
 * log goes back to standard error in the state sshd FOUND it in -- off stays
 * off, rather than every stop turning the console back on. A stop that never
 * moved the log (setup failed before it did, the image stayed resident and a
 * later `sshd stop` released it) leaves the log as it is now: @a before was
 * never recorded.
 *
 * @param serving      ///< the console take-over is (being) installed
 * @param moved        ///< the log is currently moved to the operator
 * @param have_serial  ///< the log's serial port is open
 * @param before       ///< console logging as it was before sshd served
 * @param now          ///< console logging as it is at this call
 * @return the value for axl_log_set_console_enabled()
 */
bool sshd_bind_log_console(bool serving, bool moved, bool have_serial, bool before, bool now);

/** OpenSSH's LogLevel values, in OpenSSH's order (SshdOpts::loglevel). */
typedef enum {
    SSHD_LOG_UNSET = 0,  ///< not given: AxlLog's own level (AXL_LOG_LEVEL) stands
    SSHD_LOG_QUIET,
    SSHD_LOG_FATAL,
    SSHD_LOG_ERROR,
    SSHD_LOG_INFO,
    SSHD_LOG_VERBOSE,
    SSHD_LOG_DEBUG1,     ///< also `DEBUG`, and what -v / -d set
    SSHD_LOG_DEBUG2,
    SSHD_LOG_DEBUG3,
} SshdLogLevel;

/**
 * @brief The AxlLog level an OpenSSH LogLevel sets, in either image.
 *
 * | LogLevel                | AxlLog          |
 * |-------------------------|-----------------|
 * | QUIET, FATAL, ERROR     | AXL_LOG_ERROR   |
 * | INFO                    | AXL_LOG_INFO    |
 * | VERBOSE, DEBUG, DEBUG1  | AXL_LOG_DEBUG   |
 * | DEBUG2, DEBUG3          | AXL_LOG_TRACE   |
 *
 * QUIET still logs errors, so a setup failure explains itself where
 * OpenSSH's QUIET would say nothing. VERBOSE is DEBUG, not INFO: AxlLog has
 * nothing between the two, and as INFO it would change nothing at all --
 * as DEBUG it shows the start's phase lines, the extra detail VERBOSE asks
 * for.
 *
 * @param level  ///< an SshdLogLevel
 * @return the level for axl_log_set_level(), or -1 for SSHD_LOG_UNSET (or
 *     an unknown value): set nothing, so AXL_LOG_LEVEL still decides
 */
int sshd_bind_log_level(uint32_t level);

/** Room for sshd's address list: four addresses and their separators. */
#define SSHD_ADDRS_MAX  72

/**
 * @brief Where sshd says it serves: @a ips ", "-joined into @a out.
 *
 * Only whole addresses: past the buffer the rest are counted instead, as
 * ", +N more". No address at all is "0.0.0.0", where the server listens.
 *
 * @param out  ///< [out] the list, always NUL-terminated
 * @param cap  ///< sizeof @a out (at least 16)
 * @param ips  ///< dotted-quad strings, or NULL when @a n is 0
 * @param n    ///< how many
 */
void sshd_bind_addr_list(char *out, size_t cap, const char *const *ips, size_t n);

/**
 * @brief The pre-authentication banner: the operator's `Banner` text, then
 *     the line stating the console's size.
 *
 * @a user is copied as it is (it may be empty), with a `\n` added when it
 * does not already end in one, so the size line always starts a line of its
 * own. An empty @a user gives the size line alone.
 *
 * @param out       ///< receives the banner, NUL-terminated
 * @param cap       ///< its size
 * @param user      ///< the Banner file's bytes; NULL is taken as empty
 * @param user_len  ///< how many bytes of @a user
 * @param cols      ///< the console's width
 * @param rows      ///< the console's height
 * @return the length written; 0 when it does not fit in @a cap (then
 *         nothing is written past out[0])
 */
size_t sshd_banner_compose(char *out, size_t cap, const char *user, size_t user_len,
                           uint32_t cols, uint32_t rows);

/** What a Banner file's bytes came to. */
typedef enum {
    SSHD_BANNER_OK,         ///< text to send
    SSHD_BANNER_NOT_TEXT,   ///< not UTF-8 text (bad UTF-8 or UTF-16, or a NUL)
    SSHD_BANNER_TOO_LARGE,  ///< more than the limit, as UTF-8
    SSHD_BANNER_NO_MEMORY   ///< the text could not be allocated
} SshdBannerRead;

/**
 * @brief Read a Banner file's bytes as the text sshd sends.
 *
 * The file is UTF-8 (a BOM dropped) or UTF-16LE with a BOM -- what the
 * Shell's `echo ... > file` and `edit` write -- read by axl_text_to_utf8(),
 * as sshd.conf is. The SSH banner field is UTF-8, so the text must be valid
 * UTF-8 with no NUL; and the limit is on that text, not on the file (a
 * UTF-16 file is near twice its text's size).
 *
 * @param raw      ///< the file's bytes; NULL only when @a len is 0
 * @param len      ///< their count
 * @param max      ///< the most bytes of text allowed
 * @param out      ///< receives the text, NUL-terminated, on SSHD_BANNER_OK
 *                 ///< (axl_free() it); NULL otherwise
 * @param out_len  ///< receives its length on SSHD_BANNER_OK
 * @return the outcome
 */
SshdBannerRead sshd_bind_banner_text(const void *raw, size_t len, size_t max, char **out,
                                     size_t *out_len);

/** AxlLogPort auto: the first serial port the console does not draw on. */
#define SSHD_LOG_PORT_AUTO      ((uint64_t)-1)
/** AxlLogPort auto, as AxlConfig parses it. */
#define SSHD_LOG_PORT_AUTO_STR  "18446744073709551615"
/** AxlLogPort none: no serial log while serving. */
#define SSHD_LOG_PORT_NONE      ((uint64_t)-2)

/** Where sshd's log goes while it serves. */
typedef enum {
    SSHD_LOG_TO_PORT,        ///< to the port the pick names
    SSHD_LOG_OFF_NONE,       ///< nowhere: AxlLogPort none
    SSHD_LOG_OFF_CONSOLE,    ///< nowhere: auto, and the console draws on every port
    SSHD_LOG_NO_SUCH_PORT    ///< refused: the number names a port the machine lacks
} SshdLogPick;

/**
 * @brief Pick the serial port sshd's log goes to while it serves.
 *
 * The console is never disturbed unasked: `auto` takes the first port no
 * console output device draws on, and with none such the log is not
 * written -- a serial terminal draws by absolute cursor position, so a line
 * it did not write puts everything it draws after it on the wrong row. A
 * number is the operator's explicit choice and is honoured even when the
 * console draws on that port.
 *
 * @param want     ///< AxlLogPort: SSHD_LOG_PORT_AUTO, SSHD_LOG_PORT_NONE or a number
 * @param console  ///< for each serial port, in firmware order: the console draws on it
 * @param n        ///< how many serial ports there are
 * @param index    ///< receives the port's ordinal on SSHD_LOG_TO_PORT
 * @return where the log goes
 */
SshdLogPick sshd_bind_log_port_pick(uint64_t want, const bool *console, size_t n,
                                    size_t *index);

/**
 * @brief Append one whole line to the kept log, dropping the oldest lines
 *     to make room.
 *
 * When @a line does not fit after the @a len bytes already kept, whole
 * lines are dropped from the front -- at least the older half of the
 * buffer -- so the newest lines survive and every kept line starts at a
 * line start. A line longer than @a cap is not kept. The result never
 * exceeds @a cap. Line breaks inside @a line (before its final '\\n')
 * become spaces, so one append is always one line of the text.
 *
 * @param buf   ///< the kept lines, @a cap bytes of room
 * @param cap   ///< sizeof @a buf
 * @param len   ///< bytes of whole lines now in @a buf (at most @a cap)
 * @param line  ///< the line, ending in '\\n'
 * @param n     ///< its length
 * @return the bytes kept after the append
 */
size_t sshd_bind_log_append(char *buf, size_t cap, size_t len, const char *line, size_t n);

/**
 * @brief Find the newest `[ERROR]` line among the last @a newest lines of
 *     a kept log.
 *
 * Bounded by @a len; the last line need not end in '\\n'. Lines older than
 * the last @a newest are not considered: they were written before the
 * moment the caller asks about.
 *
 * @param text      ///< the kept lines (need not be NUL-terminated)
 * @param len       ///< their length
 * @param newest    ///< how many lines at the end may be chosen
 * @param line      ///< receives the line's start
 * @param line_len  ///< receives its length, without the '\\n'
 * @return true if one was found
 */
bool sshd_bind_newest_error(const char *text, size_t len, size_t newest, const char **line,
                            size_t *line_len);

// ---------------------------------------------------------------------------
// The host key (AxlSsh P5)
// ---------------------------------------------------------------------------

/** Most HostKey lines (and --host-key flags) sshd takes. OpenSSH has no
    limit; ours travel in one fixed LoadOptions string. */
#define SSHD_HOSTKEY_MAX  8u

/** What separates the HostKey paths in SshdOpts::host_keys. FAT forbids it
    in a name, so no real path carries one; a HostKey that does is refused. */
#define SSHD_HOSTKEY_SEP  '|'

/** A HostKey entry that loads nothing (OpenSSH's `HostKey none`). */
#define SSHD_HOSTKEY_NONE  "none"

/** Why sshd is DEGRADED when its host key could not be stored: the reason it
    gives axl_service_set_degraded, and what `sshd status` prints. */
#define SSHD_HOSTKEY_DEGRADED_REASON \
    "host key not saved; it changes at every start"

/** What `sshd -T` prints as its `hostkey` when no HostKey names a file: the
    stored key, by the variable that holds it. */
#define SSHD_HOSTKEY_STORED_NAME  "uefi-variable:AxlSshHostKeyEd25519"

/**
 * @brief The next path of an SSHD_HOSTKEY_SEP-joined list.
 *
 * @a *cursor starts at the list and is advanced past what is returned; an
 * empty list has no entries. A path longer than @a cap - 1 is still
 * consumed and returned cut, which a caller sees as a file that is not
 * there.
 *
 * @return true with @a out filled, or false at the end of the list.
 */
bool sshd_bind_hostkey_next(const char **cursor, char *out, size_t cap);

#endif /* AXL_TOOLS_SSHD_BIND_H */
