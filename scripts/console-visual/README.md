# console-visual

A visual and differential test harness for anything that draws the UEFI
console. It answers one question: does what the user sees match what the
firmware is showing? `sshd` is the first suite: the live UEFI Shell over
SSH, compared against the firmware's own serial terminal and GOP screen.

This is a developer tool. It is **not** part of `run-integration.sh`. A run
exits 1 on an unclassified difference, a difference classified as a `bug`, a
step whose conditions never held, or a scenario error. A step that differs
only by classified quirks passes, and is listed as `classified only` at the
end of the run, so a quirk never goes quiet.

## Run it

```sh
scripts/console-visual/console-visual.py                  # sshd suite, x64: build, boot, all cases
scripts/console-visual/console-visual.py --only=edit,multi
scripts/console-visual/console-visual.py --keep           # leave the guest up afterwards ...
scripts/console-visual/console-visual.py --attach --only=edit   # ... and iterate with no boot
scripts/console-visual/console-visual.py --stop           # stop the kept guest
scripts/console-visual/console-visual.py --shots          # pictures for every step, not only diffs
scripts/console-visual/console-visual.py --list           # the suite's scenarios
scripts/console-visual/console-visual.py --selftest       # prove the comparison and the suite's matchers can fail (no QEMU)
```

All output goes to `out/console-visual/<suite>/`. Open `index.html` there.
The page has timing, the known-differences table, and for each step its
verdict, its pictures and its grid diff. The same data is in `summary.json`.

Each step writes these files under `<scenario>/NN-<step>[.<session>].`:

| file | what |
|---|---|
| `ssh.txt`, `ssh-sgr.txt` | the client's screen: plain text, and with SGR escapes (`tmux capture-pane`) |
| `serial.txt` | the grid oracle's screen |
| `diff.txt` | the verdict, the counts, the rows that differ side by side, the colour pairs, and which known differences matched |
| `firmware.png` | the GOP framebuffer from an HMP `screendump`, cropped to the text console |
| `ssh.png`, `serial.png` | the two grids rendered in the EFI palette |
| `side.png` | firmware next to the client |
| `diff3.png` | oracle, client, and the client with the differing cells in red |

The pictures are written only when a step differs, has a note (for example
a timeout), or `--shots` is given. Each scenario also keeps `<session>.raw`,
the exact bytes the client received (the `pipe-pane` output), and
`<session>.sent.txt`, the input that was sent.

Verdicts: `MATCH`; `DIFF-TEXT` (characters differ); `DIFF-COLOUR`; and
`DIFF-CURSOR`. A session whose client has exited is `CLOSED` and is not
compared. A step whose every difference is a known entry owned by `design`
is `BY-DESIGN` (sshd's notice to a terminal smaller than the console, held
in its place) -- but only when the client has nothing outside the console
and its cursor matches, or is owned by a matched entry. A differing cursor
is an unclassified difference unless an entry that classified the step's
cells says it owns the cursor (`owns_cursor`: a held client sits on its
notice; a log line spliced into the oracle moves the oracle's cursor). A suite may name such a held screen (`held`): a held client
counts as opened, and meets every condition that is not `client_only`.
A step whose conditions never held within its timeout fails the run too,
whatever its screens compare as: it did not show what it was written to show.

## Speed

- **One QEMU boot per run**, always through `scripts/run-qemu.sh`
  (`--background --gpu --hostfwd auto:N --extra --nsh --serial-log
  --monitor-socket`). Every scenario runs in that guest. Each scenario gets
  its own client session(s).
- **The text grids are the oracle.** A `capture-pane` or a pyte feed takes
  milliseconds. Screendumps and PNGs are made only for steps that differ.
- **Waits poll content every 50 ms**, with a bounded timeout, and never
  sleep. A step waits until its conditions hold on both the client and the
  oracle. A condition counts only after the screen has changed since the
  action. Then the step waits until every live session and the oracle have
  been unchanged for 0.3 s.
- **Each scenario starts from a known screen.** It runs the suite's `start`
  actions (`cls` for sshd), then echoes a marker of its own. So `--only=X`
  works, and `--attach` reruns a case with no boot at all.
- **Timing is in the output:** build, boot, each scenario (with QEMU's host
  CPU in cores), and the total. On an idle host, the full x64 sshd run takes
  about 80 s, of which the boot is about 10 s. With `--attach`, one case
  takes 2-20 s.

`--heartbeat FILE` appends a line every `--heartbeat-secs` (default 120).
The line gives the current step, whether QEMU is alive, and its CPU,
sampled from `/proc`.

## Layout

```
console-visual.py      CLI: loads cases/<suite>.py, boots or attaches, runs, writes the gallery
cvlib/guest.py         the session layer over run-qemu.sh: boot, keep/attach/stop, HMP over --monitor-socket
cvlib/clients.py       clients under test (Client protocol; TmuxClient runs any command in a sized tmux pane)
cvlib/oracles.py       oracles (GridOracle: SerialPyteOracle; PictureOracle: FirmwareShot)
cvlib/grid.py          the Grid/Cell model in EFI colours, and the content-gated wait
cvlib/diff.py          cell-by-cell compare, known-difference matching, diff.txt, --selftest
cvlib/render.py        grid -> PNG, side.png, the agt-style 3-up diff3.png
cvlib/runner.py        the case-table runner: actions, waits, settle, capture, timing, heartbeat
cvlib/gallery.py       index.html
cvlib/cases.py         Act / Cond / Step / Scenario / Suite: the shape of a case table
cases/sshd.py          the sshd suite: build, boot recipe, ssh client, the case table, known differences
```

## Adding a component

Write `cases/<name>.py` with a `SUITE = Suite(...)`. It supplies:

- `build(arch)`, which runs the `scripts/axl-build.sh` goal that produces your `.efi`.
- `boot(arch, workdir) -> BootSpec`: the `.efi`, the `startup.nsh` lines,
  the files to stage next to it (write them into `workdir`), any guest
  ports to forward, and the serial regex that means "ready".
- `console_size(guest)`, the console's cols x rows AT BOOT. It is usually
  parsed from your own ready line. A `cmd` action's `{boot}` is this size,
  as `COLS ROWS` (a case that reshapes the console puts it back with it).
- `client(ctx) -> Client`, the client under test. To run any command in a
  tmux pane of `ctx.size`, return a `TmuxClient(ctx.tmux, ctx.name,
  [command...], cols, rows, ctx.raw_log, tag="...")`. Examples are a raw
  VT stream read with `socat` from a `--serial-socket`, or a host-side
  renderer. A client that is not a terminal (for example a GOP-only case
  that rasterises to a Grid) implements the `Client` protocol in
  `cvlib/clients.py` directly.
- `grid_oracle` and `picture_oracle`. `SerialPyteOracle` and `FirmwareShot`
  fit any component that draws through ConOut. A component with a better
  oracle implements `GridOracle` or `PictureOracle`. The console's size can
  change under a run (a `mode` in the Shell), and a serial terminal is never
  told the new one, so `SerialPyteOracle(guest, cols, rows, geometry=RE)`
  resizes its screen wherever `RE` (a bytes pattern, groups cols and rows)
  matches in the serial stream, and removes the match -- a report your
  component writes to the serial port at the reshape, outside the console
  (sshd's is an APC string, which a terminal discards). `forbidden=RE`
  fails the run if `RE` ever reaches the serial line, which carries
  everything ConOut does: what the component must not write through the
  console. Everything else (a new session's default size, the GOP crop, the
  region compared) follows the oracle's size, not the boot's.
- `prompt` (regex), `start` (actions that reach a known screen), and
  `mark(marker)`, which returns the actions and conditions that put a
  per-scenario marker on screen.
- `cases`, the case table: `Scenario(name, what, steps, sessions=...,
  destructive=...)`. Each step is `Step(name, acts, until, timeout,
  settle)`. Actions are `cmd`, `typed`, `keys` (tmux key names), `paste`,
  `resize` (`"COLSxROWS"`, as a user resizing the window), `open` (a second
  session), and `keylat` (per-key echo timing). Conditions are `line`,
  `has`, `gone`, `joined`, `prompt`, `cursorline` (the line being edited)
  and `dead`. A condition counts only once the screen text OR the bytes
  received have changed since the action, so a step whose screen ends as it
  began (a `cls` of a screen holding only the prompt) still completes. A
  step that shows output STOPPING (a Ctrl-S halt) sets `still=SECS`: it
  completes once its conditions hold and the client and the oracle have
  both been unchanged that long, with no change required.
- `known`, the known-differences table. Each entry has an id, an owner
  (`bug`, `quirk` or `design`), a description, and a matcher over the
  differing cell. Classify each difference once, with evidence, and match
  the difference's real SHAPE, not a region: a matcher as wide as "any
  last-column cell" also hides the regressions that land there. A matched
  difference still reports `DIFF-*`, with its id in `diff.txt`, so a fix
  shows up as its entry no longer matching.
- `selftest` (optional): checks of the suite's own matchers that need no
  guest, run by `--selftest` -- what each entry must classify, and the
  near-misses it must not.

## Exploring by hand

`explore` drives a LIVE session one invocation at a time, for finding what no
scenario scripted. Each call applies its actions in command-line order, waits
for the screens to settle, then prints the ssh pane (rows numbered), the
cursor on both sides, the diff against the serial oracle (verdict, counts,
the differing rows) and the paths of the step's files.

```sh
CV="scripts/console-visual/console-visual.py explore"
$CV --start                                  # build, boot, keep the guest + sshd, open session a
$CV --type 'echo hi' --keys Enter            # act on session a; it stays open between calls
$CV --keys Up Up Enter --wait-for 'hi' --shot   # --shot: firmware + ssh PNGs and side-by-side
$CV --session b --cols 80 --rows 24 --type ls   # a second session, at its own size
$CV --paste notes.txt --sgr                  # one paste; --sgr prints the pane with its escapes (\e)
$CV --resize 70x20                           # a window-change on the open session
$CV --serial-type 'echo local' --keys Enter  # type on the guest's LOCAL keyboard (QEMU sendkey)
$CV --new-session                            # replace the session by a fresh one
$CV --close-session                          # close it after the actions
scripts/console-visual/console-visual.py --stop   # stop the guest (and every explore session)
```

- Options: `--wait-for TEXT` (a pane row contains it; `--timeout` bounds it,
  10 s) or `--settle MS` (screens unchanged that long, default 300; bounded
  at 5 s). Use `--type=-x` for text that starts with `-`.
- Sessions are tmux sessions on a private server (`cv-explore-<suite>-<arch>`),
  left open when the call ends; a session that has exited prints `CLOSED`
  until `--new-session`. The guest is the `--keep` one (`--start`, or
  `console-visual.py --keep`), found through the same state file as `--attach`.
- A new session sends Escape and `cls` first: the Shell's input line and the
  serial oracle's history are shared by every console, so without it the diff
  would be history, not the user's action. Every session shares ONE Shell.
- `--serial-type` is the QEMU keyboard, the console side the GOP shows; it
  and the serial oracle's screen are the firmware's view, ssh is the client.
- Files go to `out/console-visual/explore/<timestamp>-<n>.*` (`ssh`, `ssh-sgr`,
  `serial`, `diff`; with `--shot` also `firmware`/`ssh`/`serial`/`side`/`diff3` PNGs).
- `--selftest` includes a check that two consecutive invocations share a
  session and that actions keep their command-line order.

## Notes on the sshd suite

- The client is OpenSSH `ssh -tt` in tmux, with `TERM=xterm-256color`. tmux
  is the terminal emulator.
- Every grid is parsed by `BceScreen`, a pyte screen whose erase paints
  every cell with the current background. Plain pyte leaves cells it never
  wrote at the default colour, which made `cls 1` (a blue screen on the GOP)
  read as black on the oracle. The same holds for the line a scroll brings
  in (LF at the bottom, RI, IL, DL): pyte left it black, so every table or
  page that scrolled on `cls 1`'s blue read as a colour difference, where the
  GOP (GraphicsConsole fills it with the current background) and ssh agree.
- Colours: a glyph's foreground and background are compared; a blank
  cell's background only, since its foreground cannot be seen.
- The grid oracle is EDK2 TerminalDxe's serial stream fed into pyte. It is
  a VT encoder independent of AXL's, and it renders the same console under
  sshd's passthrough. When the oracle and the client disagree, the GOP
  screendump decides which one is right. It follows `mode` through
  `sshd --report-geometry`: inside the reshape, after TerminalDxe's own
  SetMode output and ahead of the Shell's clear, sshd writes
  `ESC _ axl-console-size=CxR ESC \` straight to the first serial port --
  the console's own UART, never ConOut -- so the GOP and the sessions are
  untouched, and the oracle resizes at that byte and drops the string. In
  band, so the resize lands at the exact byte it belongs to; asking the
  guest instead (a command, a banner fetched over a new connection) would
  change the screen or the log it is measuring. A second serial port is
  not the channel: OVMF and ArmVirtQemu bind TerminalDxe to every serial
  port they publish (a virtio console port on both arches came up as a
  `VT-UTF8`/`VT-100 Serial Console` and received the boot screen), so it
  would join the console under test, and its bytes have no place in this
  stream's order. The old way -- an INFO log line -- went through ConOut,
  onto the GOP and the serial line, from inside the SetMode it reported;
  `forbidden` fails the run if it comes back.
- The boot is the `scripts/ssh-demo.sh` recipe run through `run-qemu.sh`.
  `test/integration/lib/sshd-lib.sh` is not reused. Its boot goes through
  `common-test.sh`'s own QEMU path rather than `run-qemu.sh`, and its
  helpers type into FIFOs from bash. This harness needs a `run-qemu.sh`
  guest and tmux panes.
