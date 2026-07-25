#!/usr/bin/env python3
"""analyze-console-device-shot.py — the visual discriminator for the
axl-console-device DEBUG-OVMF smoke (test-console-device-qemu.sh).

Two modes, one per scenario in the smoke:

TAKE-OVER (default). The smoke driver renders the taken-over shell into an
80x25 GOP grid that occupies x in [0, 640), y in [0, 400). If the eviction
worked, the firmware GraphicsConsole is silent and the region at x >= 660 stays
pure black through both connect storms; if it FAILED, GraphicsConsole co-paints
the shell there. This is a host-side check because the driver, having taken over
the console, cannot itself observe whether the firmware console is also painting.
Two assertions:
  1. CLEAN: x >= CLEAN_X is essentially all black -> GraphicsConsole evicted.
  2. LIVE:  the grid region has real non-black content -> the take-over
     actually delivered the shell's output as ops.

RESTORED (--restored). The restore-variant driver takes over (wiping the frame
to pure black and rendering NOTHING -- no grid output, no status bars), then
after a few seconds uninstalls the device, which re-tags + reconnects the
firmware GraphicsConsole. The test then types `ver` so the restored console has
something to paint. If the restore worked, GraphicsConsole paints that output and
the frame is no longer black; if it FAILED, there is no console to paint it and
the frame stays black. One assertion:
  RESTORED: the frame has real non-black content -> GraphicsConsole is back and
  painting. (That the frame was black DURING take-over is proven by the take-over
  scenario, which shares the identical install path.)

PASSTHROUGH (--passthrough). The inverse of TAKE-OVER: the driver installs with
passthrough_local=true, which skips the eviction, so GraphicsConsole should keep
painting alongside us. It writes lines wider than our 80-col grid straight to
gST->ConOut, so ink past x >= 660 can only be the firmware console's.
  COPAINT: x >= CLEAN_X has content -> the local display survived our install.
  LIVE:    the grid has content    -> we are receiving the ops too (without this,
           a driver that never installed would pass COPAINT trivially).

(--wide, --input and the --fbcon* modes are documented at their check functions.)

Exit 0 on PASS, 1 on FAIL, 2 on usage error.
Usage: analyze-console-device-shot.py [--restored|--wide|--input|--passthrough|
       --fbcon|--fbcon-edit|--fbcon-ctrlc] <png>
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

GRID_W = 640                # 80 cols * 8 px
GRID_H = 400                # 25 rows * 16 px
CLEAN_X = 660               # a margin past the grid's right edge
BLACK = 24                  # per-channel value at/below which a pixel is "black"
CLEAN_MAX_STRAY = 16        # tolerated non-black px in the clean region (PNG is
                            # lossless, so this should be 0; slack is defensive)
LIVE_MIN_PIXELS = 200       # non-black px the grid must show (the shell output)
RESTORED_MIN_PIXELS = 500   # non-black px the whole frame must show once the
                            # firmware console is restored and paints `ver`


def is_black(px: tuple[int, int, int]) -> bool:
    return px[0] <= BLACK and px[1] <= BLACK and px[2] <= BLACK


def count_nonblack(
    px: object,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    stop_at: int | None = None,
) -> int:
    """Count non-black pixels in the half-open box [x0,x1) x [y0,y1). If
    stop_at is given, return as soon as the count reaches it (bounded scan)."""
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if not is_black(px[x, y]):  # type: ignore[index]
                n += 1
                if stop_at is not None and n >= stop_at:
                    return n
    return n


def check_takeover(img: Image.Image) -> int:
    w, h = img.size
    px = img.load()

    if w <= CLEAN_X:
        print(f"FAIL: framebuffer {w}x{h} too narrow for the x>{CLEAN_X} test")
        return 1

    # 1. CLEAN — the region GraphicsConsole would co-paint must be black.
    stray = 0
    first_stray: tuple[int, int] | None = None
    for y in range(h):
        for x in range(CLEAN_X, w):
            if not is_black(px[x, y]):  # type: ignore[index]
                stray += 1
                if first_stray is None:
                    first_stray = (x, y)
    clean_ok = stray <= CLEAN_MAX_STRAY

    # 2. LIVE — the grid must actually show the taken-over shell's output.
    live = count_nonblack(px, 0, 0, min(GRID_W, w), min(GRID_H, h),
                          stop_at=LIVE_MIN_PIXELS)
    live_ok = live >= LIVE_MIN_PIXELS

    print(f"  framebuffer      : {w}x{h}")
    print(f"  clean region x>={CLEAN_X}: {stray} non-black px "
          f"(<= {CLEAN_MAX_STRAY} required)"
          + (f", first at {first_stray}" if first_stray else ""))
    print(f"  grid content     : {'>=' if live_ok else '<'} {LIVE_MIN_PIXELS} "
          f"non-black px (need >= {LIVE_MIN_PIXELS})")

    if clean_ok and live_ok:
        print("PASS: take-over is clean (GraphicsConsole evicted) and the grid "
              "shows the shell output")
        return 0
    if not clean_ok:
        print("FAIL: the x>640 region is NOT black -- GraphicsConsole is "
              "co-painting, eviction did not take")
    if not live_ok:
        print("FAIL: the grid is (nearly) empty -- the take-over did not deliver "
              "the shell's output as ops")
    return 1


def check_restored(img: Image.Image) -> int:
    w, h = img.size
    px = img.load()

    # The restore driver leaves the frame pure black; a working restore lets the
    # firmware console paint `ver` back onto it. Any real content = restored.
    content = count_nonblack(px, 0, 0, w, h, stop_at=RESTORED_MIN_PIXELS)
    ok = content >= RESTORED_MIN_PIXELS

    print(f"  framebuffer      : {w}x{h}")
    print(f"  restored content : {'>=' if ok else '<'} {RESTORED_MIN_PIXELS} "
          f"non-black px (need >= {RESTORED_MIN_PIXELS})")

    if ok:
        print("PASS: the firmware console is restored and painting (uninstall "
              "re-tagged + reconnected GraphicsConsole)")
        return 0
    print("FAIL: the frame is (nearly) black after uninstall -- the firmware "
          "console was NOT restored, so its output goes nowhere")
    return 1


def check_passthrough(img: Image.Image) -> int:
    """PASSTHROUGH mode: the exact inverse of check_takeover's clean assertion.

    The passthrough driver installs with passthrough_local=true, which skips the
    eviction step, so ConSplitter should fan console output to BOTH our 80x25 grid
    and the firmware GraphicsConsole. It then drives gST->ConOut directly with lines
    wider than 80 columns. Our grid clamps at x<640, so ink beyond CLEAN_X can only
    have been painted by the firmware console -- which is precisely the evidence
    check_takeover demands be ABSENT. Two assertions:
      1. COPAINT: x >= CLEAN_X has real content -> GraphicsConsole is still in the
         fan-out, i.e. the local display survived our install.
      2. LIVE:    the grid region has content -> we are ALSO receiving the ops, so
         this is a genuine co-paint and not simply a failed/absent install.
    Assertion 2 is what makes assertion 1 meaningful: a driver that never installed
    would leave the firmware console painting happily and pass assertion 1 alone."""
    w, h = img.size
    px = img.load()

    if w <= CLEAN_X:
        print(f"FAIL: framebuffer {w}x{h} too narrow for the x>{CLEAN_X} test")
        return 1

    copaint = count_nonblack(px, CLEAN_X, 0, w, h, stop_at=LIVE_MIN_PIXELS)
    copaint_ok = copaint >= LIVE_MIN_PIXELS

    live = count_nonblack(px, 0, 0, min(GRID_W, w), min(GRID_H, h),
                          stop_at=LIVE_MIN_PIXELS)
    live_ok = live >= LIVE_MIN_PIXELS

    print(f"  framebuffer      : {w}x{h}")
    print(f"  co-paint x>={CLEAN_X}  : {'>=' if copaint_ok else '<'} {LIVE_MIN_PIXELS} "
          f"non-black px (need >= {LIVE_MIN_PIXELS})")
    print(f"  grid content     : {'>=' if live_ok else '<'} {LIVE_MIN_PIXELS} "
          f"non-black px (need >= {LIVE_MIN_PIXELS})")

    if copaint_ok and live_ok:
        print("PASS: passthrough co-paints -- the firmware console still paints the "
              "local display AND our device still receives the ops")
        return 0
    if not copaint_ok:
        print(f"FAIL: x>={CLEAN_X} is black -- the firmware console is NOT painting, "
              "so passthrough_local did not keep it in the fan-out (the local "
              "display would be dead)")
    if not live_ok:
        print("FAIL: the grid is (nearly) empty -- our device received no ops, so "
              "any firmware painting proves nothing about passthrough")
    return 1


# The wide smoke's grid is 142 cols * an 8px cell = 1136 px wide. Two regions:
WIDE_GRID_RIGHT = 1136   # right edge of OUR 142-col grid
WIDE_EVICT_X    = 1160   # past the grid, with margin: must stay black if we evicted


def check_wide(img: Image.Image) -> int:
    """WIDE mode: the take-over console is advertised at a NON-80x25 geometry
    (142x44), so the shell renders PAST the 80-col / x=640 boundary. Two
    assertions, together specific to "OUR device rendered the wide grid":
      1. WIDE:   x in [CLEAN_X, WIDE_GRID_RIGHT) has real content -> the wide
         geometry is actually active (QueryMode/Mode honor cfg; a regression to a
         hardcoded 80x25 leaves this region black and fails).
      2. EVICTED: x >= WIDE_EVICT_X (past OUR 142-col grid) is black -> GraphicsConsole
         was evicted and it's OUR grid painting, not the firmware co-painting a wide
         native console (which would happen if install rejected the wide cfg and the
         driver unloaded -- that would otherwise pass assertion 1 spuriously).
    (Assertion 1 keys on non-black content, which assumes the shell's dh runs on a
    black background -- true for the UEFI shell.)"""
    w, h = img.size
    px = img.load()
    if w <= WIDE_EVICT_X:
        print(f"FAIL: framebuffer {w}x{h} too narrow for the wide + evict test")
        return 1

    wide = count_nonblack(px, CLEAN_X, 0, min(WIDE_GRID_RIGHT, w), h,
                          stop_at=LIVE_MIN_PIXELS)
    wide_ok = wide >= LIVE_MIN_PIXELS

    stray = 0
    first_stray: tuple[int, int] | None = None
    for y in range(h):
        for x in range(WIDE_EVICT_X, w):
            if not is_black(px[x, y]):  # type: ignore[index]
                stray += 1
                if first_stray is None:
                    first_stray = (x, y)
    evict_ok = stray <= CLEAN_MAX_STRAY

    print(f"  framebuffer      : {w}x{h}")
    print(f"  wide content [{CLEAN_X},{WIDE_GRID_RIGHT}): {'>=' if wide_ok else '<'} "
          f"{LIVE_MIN_PIXELS} non-black px (need >= {LIVE_MIN_PIXELS})")
    print(f"  evicted x>={WIDE_EVICT_X}: {stray} non-black px (<= {CLEAN_MAX_STRAY} "
          f"required)" + (f", first at {first_stray}" if first_stray else ""))

    if wide_ok and evict_ok:
        print("PASS: OUR grid renders the shell past x=640 (wide geometry active) "
              "and GraphicsConsole is evicted past the grid")
        return 0
    if not wide_ok:
        print("FAIL: the [660,1136) grid region is empty -- the shell is NOT using "
              "the wide geometry (QueryMode may have regressed to a hardcoded 80x25)")
    if not evict_ok:
        print("FAIL: content past our grid (x>=1160) -- the firmware is co-painting "
              "a wide console (take-over failed), so wide content is not proof of ours")
    return 1


# INPUT mode geometry: the ver OUTPUT sits below the load-success line (row 0) and
# the echoed prompt (row 1). If keys never reached the shell (the relay failed) the
# command never runs and this region stays blank -- so content here is proof the
# read-loop -> inject -> shell path delivered the keystrokes.
INPUT_OUT_Y0 = 32    # row 2 (below the load line + the "FS0:\> ver" prompt echo)
INPUT_OUT_Y1 = 112   # rows 2..6, covering ver's multi-line output


def check_input(img: Image.Image) -> int:
    """INPUT mode: the smoke installs take_input=true (evicting the raw keyboard)
    and we --sendkey `ver`. The ONLY path from that keystroke to the shell is our
    read-loop -> inject -> ConInEx relay, so:
      1. CLEAN:  x >= CLEAN_X black -> the (output) take-over evicted GraphicsConsole.
      2. RAN:    the ver OUTPUT region (rows 2..6, x < 640) has real content -> the
         command actually executed, i.e. our relay delivered the keystrokes and
         there was no double-delivery (a doubled 'vveerr' would not be a valid
         command, so no output). A blank region = keys never arrived."""
    w, h = img.size
    px = img.load()
    if w <= CLEAN_X:
        print(f"FAIL: framebuffer {w}x{h} too narrow for the x>{CLEAN_X} test")
        return 1

    stray = 0
    first_stray: tuple[int, int] | None = None
    for y in range(h):
        for x in range(CLEAN_X, w):
            if not is_black(px[x, y]):  # type: ignore[index]
                stray += 1
                if first_stray is None:
                    first_stray = (x, y)
    clean_ok = stray <= CLEAN_MAX_STRAY

    ran = count_nonblack(px, 0, INPUT_OUT_Y0, min(GRID_W, w),
                         min(INPUT_OUT_Y1, h), stop_at=LIVE_MIN_PIXELS)
    ran_ok = ran >= LIVE_MIN_PIXELS

    print(f"  framebuffer      : {w}x{h}")
    print(f"  clean region x>={CLEAN_X}: {stray} non-black px "
          f"(<= {CLEAN_MAX_STRAY} required)"
          + (f", first at {first_stray}" if first_stray else ""))
    print(f"  ver output [rows 2..6]: {'>=' if ran_ok else '<'} {LIVE_MIN_PIXELS} "
          f"non-black px (need >= {LIVE_MIN_PIXELS})")

    if clean_ok and ran_ok:
        print("PASS: the injected `ver` ran and painted its output -> our input "
              "relay delivered the keystrokes (raw keyboard evicted, no double-delivery)")
        return 0
    if not clean_ok:
        print("FAIL: the x>640 region is NOT black -- the output take-over did not take")
    if not ran_ok:
        print("FAIL: the ver-output region is blank -- the injected keystrokes did "
              "NOT reach the shell (the input relay did not deliver)")
    return 1


FBCON_MIN_PIXELS = 2000   # a full-screen terminal render lights far more than this;
                          # a dead/black frame (install or render failed) lights ~0


def check_fbcon(img: Image.Image) -> int:
    """FBCON mode: unlike the partial-grid smokes, `fbcon` is a FULL-SCREEN
    terminal -- its cell grid covers the whole framebuffer, so the "x>640 must be
    black" eviction discriminator does NOT apply (fbcon legitimately paints edge to
    edge). The discriminator here is simply that the terminal rendered a screenful of
    the taken-over shell: substantial non-black content across the WHOLE frame. Paired
    with run_scenario's zero-firmware-fatals check, a pass proves the take-over +
    AxlConsoleTerm render + input-relay/key_filter + uninstall pipeline runs fatal-free
    in real firmware and paints the shell. Fine-grained keystroke-delivery
    discrimination and pointer-driven selection/zoom stay unit-tested (a full-screen
    render can't localize the `ver` output region, and QEMU can't drive the pointer)
    -- a disclosed firmware-coverage gap, per the design doc."""
    w, h = img.size
    px = img.load()
    content = count_nonblack(px, 0, 0, w, h, stop_at=FBCON_MIN_PIXELS)
    ok = content >= FBCON_MIN_PIXELS
    print(f"  framebuffer      : {w}x{h}")
    print(f"  frame content    : {'>=' if ok else '<'} {FBCON_MIN_PIXELS} "
          f"non-black px (need >= {FBCON_MIN_PIXELS})")
    if ok:
        print("PASS: fbcon rendered the taken-over shell full-screen (terminal active, "
              "no firmware fatals)")
        return 0
    print("FAIL: the frame is (near) black -- fbcon did not take over / render the "
          "shell")
    return 1


# EDIT-TYPE mode: after fbcon takes over we --sendkey `edit <ret>` then a run of
# letters. edit's text area starts at row 1 (y[16,32)); the typed line renders there.
# Col 0 (x[0,8)) can carry edit's parked mouse-cursor block, so we skip it and key on
# cols 1+ -- content there is proof the keystrokes reached edit and it rendered them.
# This guards the pointer-proxy regression where a spurious mouse "activity" starved
# edit's keyboard poll so typed characters never appeared.
EDIT_ROW_Y0 = 16
EDIT_ROW_Y1 = 32
EDIT_TEXT_X0 = 8         # skip col 0 (edit's mouse-cursor block)
EDIT_MIN_PIXELS = 40     # a run of typed glyphs lights far more; an empty line ~0


def check_fbcon_edit(img: Image.Image) -> int:
    """EDIT-TYPE mode: proves a guest running UNDER fbcon (the UEFI `edit`) receives
    and renders typed keystrokes. If the text area (row 1, cols 1+) has real content
    the characters got through; a blank line means edit never saw them."""
    w, h = img.size
    px = img.load()
    typed = count_nonblack(px, EDIT_TEXT_X0, EDIT_ROW_Y0, min(GRID_W, w),
                           min(EDIT_ROW_Y1, h), stop_at=EDIT_MIN_PIXELS)
    ok = typed >= EDIT_MIN_PIXELS
    print(f"  framebuffer          : {w}x{h}")
    print(f"  edit text row (cols 1+): {'>=' if ok else '<'} {EDIT_MIN_PIXELS} "
          f"non-black px (need >= {EDIT_MIN_PIXELS})")
    if ok:
        print("PASS: typed characters rendered in edit's text area -> the keystrokes "
              "reached the guest under fbcon (pointer proxy did not starve the keyboard)")
        return 0
    print("FAIL: edit's text area is blank -- typed characters never reached the guest "
          "(regression: the pointer proxy starved edit's keyboard poll)")
    return 1


# CTRL-C mode: after fbcon takes over we --sendkey `echo 1 <ret>`, then a guest
# Ctrl+C, then several more `echo N <ret>` lines. A guest Ctrl+C signals the Shell's
# ExecutionBreak; fbcon's render loop must NOT treat that as its own quit (it opts out
# of the loop's Ctrl+C=quit interception). If it DID quit, the render/pointer pump
# stops: the frame freezes right after `echo 1`'s output (rows 0..3) and every later
# line -- echoed input AND command output -- never paints. So content BELOW the freeze
# point (row 5+, y>=80) is proof the loop survived the Ctrl+C and kept rendering.
CTRLC_LIVE_Y0 = 80        # row 5+: below `echo 1` output + the prompt that froze on regression
CTRLC_LIVE_Y1 = 256
CTRLC_MIN_PIXELS = 100    # the post-Ctrl+C echo lines light far more; a frozen frame = 0


def check_fbcon_ctrlc(img: Image.Image) -> int:
    """CTRL-C mode: proves a guest Ctrl+C does not freeze fbcon's render loop. The
    post-Ctrl+C `echo` lines paint below the freeze point only if the loop kept
    pumping; a blank lower region means the loop quit on the Shell's ExecutionBreak."""
    w, h = img.size
    px = img.load()
    live = count_nonblack(px, 0, CTRLC_LIVE_Y0, min(GRID_W, w),
                          min(CTRLC_LIVE_Y1, h), stop_at=CTRLC_MIN_PIXELS)
    ok = live >= CTRLC_MIN_PIXELS
    print(f"  framebuffer          : {w}x{h}")
    print(f"  post-Ctrl+C rows (y>={CTRLC_LIVE_Y0}): {'>=' if ok else '<'} "
          f"{CTRLC_MIN_PIXELS} non-black px (need >= {CTRLC_MIN_PIXELS})")
    if ok:
        print("PASS: output kept rendering after a guest Ctrl+C -> fbcon's render loop "
              "survived the Shell's ExecutionBreak (did not self-quit)")
        return 0
    print("FAIL: the frame froze after Ctrl+C -- fbcon's render loop quit on the Shell's "
          "ExecutionBreak (regression: intercept_break not disabled)")
    return 1


def main() -> int:
    args = sys.argv[1:]
    mode = "takeover"
    if args and args[0] in ("--restored", "--wide", "--input", "--passthrough",
                            "--fbcon", "--fbcon-edit", "--fbcon-ctrlc"):
        mode = args[0][2:]
        args = args[1:]
    if len(args) != 1:
        print("usage: analyze-console-device-shot.py "
              "[--restored|--wide|--input|--passthrough|--fbcon|--fbcon-edit|"
              "--fbcon-ctrlc] <png>", file=sys.stderr)
        return 2

    path = Path(args[0])
    if not path.is_file():
        print(f"FAIL: screenshot not found: {path}")
        return 1

    img = Image.open(path).convert("RGB")
    if mode == "restored":
        return check_restored(img)
    if mode == "wide":
        return check_wide(img)
    if mode == "input":
        return check_input(img)
    if mode == "passthrough":
        return check_passthrough(img)
    if mode == "fbcon":
        return check_fbcon(img)
    if mode == "fbcon-edit":
        return check_fbcon_edit(img)
    if mode == "fbcon-ctrlc":
        return check_fbcon_ctrlc(img)
    return check_takeover(img)


if __name__ == "__main__":
    raise SystemExit(main())
