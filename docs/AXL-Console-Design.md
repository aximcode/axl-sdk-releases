# AXL Console — Design (umbrella)

**Status:** MIXED — `axl-console`, `axl-console-tap`, `axl-console-mirror`,
`axl-vterm` are SHIPPED; `axl-console-device` (the take-over producer) is
**PROVEN by spike, not yet an extracted `<axl/…>` surface** — its output
take-over is captured on both arches (`AXL-Console-Device-Design.md`). This is
the **start-here** map for the console family. It owns the cross-cutting parts
(the `AxlConsoleOps` contract and the producer/consumer + insertion-strategy
architecture); each interface's details live in its own `AXL-Console-*-Design.md`
leaf.

The family exists to solve one problem: **let an AXL/AGT program observe and
drive the real firmware console** — the UEFI Shell and its full-screen tools
(`edit`) — so the console can be mirrored to a remote terminal (SoftBMC) or
rendered locally in a GOP terminal widget (AGT), while input is relayed from an
alternate source.

**Why it started — the key bounce.** The concrete motivation is a defect: keys
**repeat / bounce** when you work directly at the raw firmware Shell prompt over
a laggy BMC/KVM link. `axedit` (the AGT editor) on the *same* link does **not**
bounce — and the `kbtune` hardware research (axl-sdk `AXL-KbTune-Design.md` §7)
pinned that it stays clean **with the software debounce OFF**. So axedit's
immunity is **architectural** (its read + render path), **not** a debounce
filter. *Which* axis is still open (kbtune §7): drain cadence is **ruled weak**,
the **damaged-region redraw cadence is the leading candidate**, and input
read-timing is unexplored. The consequence for this family: the **render
take-over** is the *plausible* bounce cure — the take-over device (`axl-console-device`,
proven by spike) already does it — and the debounced-input path is **not**
established as the fix. So the goal is "give the Shell AGT's read + render path";
the render half is proven and plausibly the cure, the input relay is **built**
(`axl-console-device` increment 2: input ownership + Ctrl+letter, with an
OFF-by-default key-repeat gate) but its bounce effect is **unproven** — the kbtune
A/B (shell-greedy vs drain-all-filter-off, on a bouncing link) is what settles
which half, if either, actually meters the burst.

---

## 1. The contract: `AxlConsoleOps`

Everything hangs off one C vtable, `AxlConsoleOps`: a **structured** view of the
console as operations (`clear_screen`, `set_cursor`, `output_text`, `set_pen`,
`erase`, `scrollrect`, `set_term_prop`, …), *not* a VT/ANSI byte stream.
`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` has no wire format — a full-screen app just
calls `SetCursorPosition`/`OutputString`/`SetAttribute` — so structured ops are
the honest, lossless representation. A *remote* consumer serializes them to a VT
stream; a *local* consumer binds them straight into a cell grid, with no
encode→parse round trip and state (the alternate screen) the VT wire can only
approximate.

Two of the ops' `int` returns are **load-bearing** (the rest only gate a debug
log): `scrollrect` (drives the `moverect`+`erase` decomposition) and
`set_term_prop` (libvterm only *stores* a property if the callback accepted it —
reject `ALT_SCREEN` and the parser desyncs). See the tap/mirror leaves.

**⚠ Contract location (a known wart).** `AxlConsoleOps` and its shared types
(`AxlConsolePen`, `AxlConsoleColor`, `AxlConsoleRect`, `AxlConsoleCellRule`, …)
currently live *inside* `axl-console-tap.h`, coupled to one producer. Extracting
them into a standalone `axl-console-ops.h` that every producer and consumer
includes is a planned refactor (so `axl-console-device` need not include the
*tap* header just for the contract). Bundle it with the `axl-console-device`
extraction (the take-over producer being lifted out of the spike), not
speculatively.

---

## 2. Architecture: producers, consumers, insertion strategies

```
   producers (feed ops)                          consumers (drain ops)
   ┌─────────────────────┐                        ┌──────────────────────┐
   │ axl-console-tap     │──┐                  ┌──│ axl-console-mirror   │→ VT wire (SoftBMC)
   │ axl-console-device  │──┤► AxlConsoleOps ◄─┤  │ AgtTerminal (AGT)    │→ GOP cell grid
   │ axl-vterm           │──┘                  └──│ ...                  │
   └─────────────────────┘                        └──────────────────────┘
```

- **Two-producers/one-contract.** The same ops flow into any consumer regardless
  of where they came from. A grid consumer (AGT) and a VT consumer (mirror)
  differ only in how they drain the ops.
- **Insertion strategy** is a property of the producers that *wrap a real
  firmware console* (`tap`, `device`): *how* they splice into the console
  topology to see the Shell's output and inject its input. This is the axis the
  console-provider work is about — see §4.
- **No pump of their own.** Producers do not create a loop; the caller drives one
  (`axl_loop_attach_driver` in a resident driver, or a foreground app's own
  loop). `axl-vterm` is pump-free entirely (a synchronous `feed()`).

---

## 3. The family map

| Interface | Role | Insertion / direction | Status | Leaf doc |
|---|---|---|---|---|
| `axl-console-tap` | producer | wraps `gST->ConOut/ConIn` by **swapping the pointers** | SHIPPED | `AXL-Console-Tap-Design.md` |
| `axl-console-device` | producer | registers + evicts a **ConSplitter device** (take-over, no swap) | PROVEN (spike; extraction pending) | `AXL-Console-Device-Design.md` |
| `axl-vterm` | producer | **VT byte stream** → ops (libvterm Layer 2, vendored) | SHIPPED | — (see `axl-vterm.h`) |
| `axl-console-mirror` | consumer | ops → **VT wire** for a remote terminal (SoftBMC) | SHIPPED | `AXL-Console-Mirror-Design.md` |
| `axl-console` | *adjacent* | direct interactive-console API (read key/line, text-mode geometry) — **not** the ops contract | SHIPPED | — (see `axl-console.h`) |

`axl-console.h` is in the family by name but is a *different concern*: a
convenience API for a program that drives the console itself (blocking key/line
reads, page-break, mode geometry), not interposition. Listed here so readers do
not mistake it for the ops contract.

---

## 4. Insertion strategies (the console-provider axis)

A producer that wraps a live firmware console must choose where to splice in. The
UEFI console is already a fan-out: the Shell talks to one virtual console
(`gST->ConOut`, `ConsoleInHandle`) and **ConSplitterDxe** fans output to N
console-out devices and merges N console-in devices.

- **`axl-console-tap` — swap `gST->ConOut`.** Replace the top-level pointer.
  Simple and high-fidelity, but the swap is **only safe when it predates the
  Shell's per-command save/restore bracket**: a foreground app that then
  `StartImage`s a child `Shell.efi` (axterm), or a BDS-time driver. Swapping from
  a resident driver's `DriverEntry` into an *already-running* shell deadloops —
  the Shell's `RestoreStdInStdOutStdErr` frees the swapped struct
  (`ShellParametersProtocol.c:1420` → `ConsoleWrappers.c:521` →
  `Pool.c(721)` assert). (Full mechanism: AGT `spike/README.md` +
  `.superpowers/sdd/edk2-mechanism.md`.)
- **`axl-console-device` — register a ConSplitter virtual device (take-over).**
  **PROVEN, and the primary local producer.** Install our own
  `SimpleTextOut`/`SimpleTextInputEx`(/`AbsolutePointer`) as a console device and
  **evict the other console devices** (uninstall their console-device tag) so the
  Shell's output fans **only** to us — sole ownership of the framebuffer with **no
  child shell**. **`gST->ConOut` never changes value**, so the Shell-bracket free
  never fires — deterministic, no timing race, and full-screen input works
  natively via `ConsoleInHandle`. This is the strategy for a *resident, no-child*
  console provider, and the spike (`agt/spike/conprov-cv2.c`) proved clean local
  ownership on both arches with zero NVRAM writes. Mechanism + evidence:
  `AXL-Console-Device-Design.md`.

Both strategies feed the **same** `AxlConsoleOps`, so a consumer (AGT, mirror) is
unaffected by which one is used. Which strategy a deployment picks follows from
its **launch model** — the forkpty/reptyr split in §5.

---

## 5. The console as a pseudo-terminal (PTY / forkpty / reptyr)

The clearest way to understand this family is by analogy to the POSIX
pseudo-terminal, because every piece has a kin there — and the one place the
analogy *breaks* is what shapes the whole contract.

**The mapping.**

| POSIX | AXL / UEFI |
|---|---|
| **slave** (`/dev/pts/N`, the TTY a program does I/O on) | `gST->ConOut` / `ConsoleInHandle` — what the Shell and `edit` talk to |
| **master + terminal emulator** (intercept output, inject input, render) | our `axl-console-tap` / `axl-console-device` |
| **`forkpty(3)`** = `openpty` + fork the child onto the slave | **axterm** — swap the console, then `StartImage` a child `Shell.efi` |
| **`register_console()` + unregister `fbcon`** (steal the *one* system console) | **take-over** (`axl-console-device`) — become the sole console device, evict the firmware one |
| **`reptyr`** — steal a terminal from an *already-running* process | the take-over's whole point: interpose on the *running* boot shell |

`forkpty` is the child-shell shape (we own the foreground, spawn the child onto
our slave). Take-over is the `register_console`/reptyr shape: UEFI has **one
global console** plus a **runtime-reconfigurable router — ConSplitter — that
Linux lacks per-process**, so we can register a new console device and evict the
firmware's, seizing a shell we did not launch. Two launch models, one contract:

| Launch context | Shape | POSIX kin |
|---|---|---|
| **Loaded onto the existing ESP boot shell** (resident driver) — **primary** | **take-over** (`axl-console-device`) | `register_console()` / reptyr |
| **ESP boot app** (`\EFI\BOOT\BOOTx64.EFI`), we are the foreground | **child-shell** (axterm, `axl-console-tap`) | `forkpty(3)` |

**The key difference that shapes the API.** A PTY is a **byte pipe + a line
discipline**: terminal semantics travel *in-band*, as VT escape sequences the
emulator parses out of the byte stream. UEFI `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`
is the opposite — a **structured, out-of-band API**: `SetCursorPosition` is a
*call*, not an `ESC[…H` in the stream. It is **Windows-Console-API-shaped, not
Unix-PTY-shaped** (Windows itself only grew a PTY — ConPTY — in 2018). This is
exactly why `AxlConsoleOps` is a **structured op stream** and not a byte sink,
and why axterm could **delete its VT parser** and bind ops straight into the
grid (`AGT-Axterm-Design.md` §P6).

**Where the family members fall out of the lens.**

- **`axl-vterm`** = the VT-byte-stream → ops parser — the piece a *Unix*
  terminal emulator needs. It lets a genuine *byte* source (serial / SOL / a
  remote xterm keydown stream) drive the same `AxlConsoleOps` grid a structured
  producer feeds.
- **`axl-console-mirror`** = the reverse, ops → VT bytes — serialize our
  structured ops onto a wire for a remote xterm-class terminal, exactly like a
  PTY master forwarding the slave's output to `sshd`.
- **The POSIX line discipline** (echo, `ISIG` / Ctrl+C, Ctrl+letter folding,
  cooked mode) has **no UEFI equivalent.** The Shell plus our injection
  re-implement only the bits we need — which is precisely the deferred
  **Ctrl+Q / Ctrl+letter** conditioning (`axl-console-tap` §5,
  `axl-console-device` §5).

---

## 6. Where the pieces live

- Contract + producers/consumers: `src/util/axl-console*.c`,
  `include/axl/axl-console*.h`; `axl-vterm` under the same prefix; libvterm
  vendored in `deps/libvterm/`.
- Consumers outside axl-sdk: SoftBMC (via `axl-console-mirror`) and AGT's
  `AgtTerminal` (via the tap today; the device strategy when it lands).

## 7. Read next

- `AXL-Console-Tap-Design.md` — the shipped gST-swap producer (the forkpty /
  child-shell sibling) + the full `AxlConsoleOps` reference.
- `AXL-Console-Device-Design.md` — the take-over producer (register + evict a
  ConSplitter device; the reptyr shape). PROVEN by spike; the primary local
  strategy.
- `AXL-Console-Mirror-Design.md` — the shipped VT-wire consumer (SoftBMC).
- `axl-vterm.h` — the VT-stream producer.
- AGT `AGT-Console-Relay-Design.md` — the AGT-side adoption stub for the
  take-over device (`AgtTerminal` binds `AxlConsoleOps`).
