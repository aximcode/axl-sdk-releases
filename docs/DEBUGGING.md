# Debugging AXL Apps Under QEMU + GDB

How to attach GDB to a running QEMU+OVMF guest, load matching debug
symbols for both EDK2 firmware modules and the AXL EFI under test,
and step through code at source level.

Last updated: April 2026

---

## When to use this

Reach for GDB-into-QEMU when:

- Heavy `axl_info` / printf instrumentation perturbs timing enough to
  hide the bug (Heisenbugs in event-loop / TCP timing).
- The bug is firmware-side (DxeCore, MnpDxe, Tcp4Dxe) — `axl_info`
  doesn't reach there.
- You need the call stack at a specific moment (e.g. who closed
  this event handle, what TPL is gBS->SignalEvent running at).

For ordinary bring-up bugs in AXL code, `axl_info` and the unit
tests are still the faster path. GDB has setup overhead.

---

## One-time setup

### 1. DEBUG-build OVMF with debug symbols

You need an OVMF firmware built with `-DDEBUG_LEVEL=...` such that
`Loading driver at 0x... NAME.efi` lines reach the debug console,
plus the matching `*.debug` ELF files.

The known-good local build is at:

```
firmware:  /home/mgosha/uefi/build/firmware/OVMF_{CODE,VARS}.fd
symbols:   /home/mgosha/uefi/Build/OvmfX64/DEBUG_GCC5/X64/*.debug
```

The system `edk2-ovmf` package is built without those symbols and
also triggers a separate firmware-version `#GP` regression in
test-http; do not use it.

### 2. EDK2 source on disk

GDB resolves source paths from DWARF (`/home/mgosha/uefi/edk2/...`).
The matching tree lives at `/home/mgosha/projects/edk2`. Add it to
GDB with:

```
(gdb) directory /home/mgosha/projects/edk2
```

Or set in `~/.gdbinit`. The TCP path AXL exercises is in
`NetworkPkg/TcpDxe/`, the event/TPL machinery is in
`MdeModulePkg/Core/Dxe/Event/`, and the loader is in
`MdeModulePkg/Core/Dxe/Image/`.

### 3. QEMU build

The `--gdb` integration assumes a QEMU that supports the standard
`-gdb tcp::PORT` and `-debugcon`. The locally built QEMU at
`/home/mgosha/projects/qemu/qemu-10.0.0/build/qemu-system-x86_64`
is known-good.

---

## Standalone EFI debugging

Use this when you can boot the EFI directly without DHCP / network
setup (most unit-test EFIs, pure-CPU tests, mem/data tests).

```bash
QEMU_BIN=/home/mgosha/projects/qemu/qemu-10.0.0/build/qemu-system-x86_64 \
OVMF_CODE=/home/mgosha/uefi/build/firmware/OVMF_CODE.fd \
OVMF_VARS=/home/mgosha/uefi/build/firmware/OVMF_VARS.fd \
./scripts/run-qemu.sh --gdb --background \
    --serial-log /tmp/srv.log --debugcon /tmp/dc.log \
    out/native-x64/AxlTestCpuIdle.efi
```

Flags:

| Flag | Effect |
|------|--------|
| `--gdb [PORT]` | Expose QEMU GDB stub on `tcp::PORT` (default 1234), free-running boot |
| `--gdb-halt` | With `--gdb`, also pass `-S` (halt before instruction 0) — for SecMain/PEI debugging |
| `--debugcon FILE` | Capture OVMF DEBUG output (port 0x402). Required for `gdb-syms.py` to recover module load addresses |

Notes:

- `--gdb` drops `-enable-kvm` / `-cpu host` from the QEMU command —
  KVM is incompatible with single-stepping early boot instructions.
  TCG is slower but needed for breakpoint reliability.
- The `Loading driver at 0x... NAME.efi` lines do NOT appear on the
  regular serial console for this OVMF build — they only land on
  port 0x402, which `--debugcon` captures.

---

## Live integration-test debugging

Use this when the bug only reproduces under the actual harness (DHCP
running, MNP/Ip4/TcpDxe bound, host-side curl traffic).

```bash
QEMU_BIN=... OVMF_CODE=... OVMF_VARS=... \
TEST_QEMU_GDB=1234 TEST_QEMU_DEBUGCON=/tmp/dc.log \
    ./test/integration/test-http.sh
```

The two `TEST_QEMU_*` env vars are read by `common-test.sh` and
add `-gdb` / `-debugcon` to the harness's QEMU command without
changing any other test behaviour. The test still runs to
completion (or timeout); attach GDB during the test phase.

---

## Symbol loading: `scripts/gdb-syms.py`

GDB needs to know the runtime image base of every module it has
symbols for. The debugcon log tells us those bases. The script:

```bash
./scripts/gdb-syms.py /tmp/dc.log \
    --build-dir /home/mgosha/uefi/Build/OvmfX64/DEBUG_GCC5/X64 \
    --axl-build-dir out/native-x64 \
    > /tmp/syms.gdb
```

reads the debugcon log, finds the matching `<NAME>.debug` for every
loaded driver/PEIM, computes the runtime `.text` VMA, and prints
`add-symbol-file` lines to stdout. Pass `--axl-build-dir` to also
load AXL test EFIs (which use a different `.text` offset than EDK2
modules — the script reads it from the sibling `.so`).

Then attach GDB:

```bash
gdb -nx \
  -ex 'set pagination off' \
  -ex 'target remote :1234' \
  -ex 'source /tmp/syms.gdb'
```

`source /tmp/syms.gdb` only does the symbol loading. It does not set
breakpoints or continue. You drive the rest interactively.

---

## Common breakpoints

### AXL code

| Function | File | Notes |
|----------|------|-------|
| `axl_tcp_close` | src/net/axl-tcp-sync.c | Sync close path |
| `axl_loop_dispatch_event` | src/loop/axl-loop.c | Per-event dispatch |
| `axl_backend_event_create` | src/backend/native/axl-backend-native-event.c | Wraps gBS->CreateEvent |
| `axl_backend_event_close_dbg` | src/backend/native/axl-backend-native-event.c | Wraps gBS->CloseEvent + records double-close ring |

### EDK2 firmware

| Function | File | Purpose |
|----------|------|---------|
| `CoreSignalEvent` | MdeModulePkg/Core/Dxe/Event/Event.c:526 | Implementation behind gBS->SignalEvent |
| `CoreCloseEvent`  | MdeModulePkg/Core/Dxe/Event/Event.c | Implementation behind gBS->CloseEvent |
| `CoreWaitForEvent`| MdeModulePkg/Core/Dxe/Event/Event.c | gBS->WaitForEvent — useful for HLT-vs-spin debugging |
| `Tcp4Close`       | NetworkPkg/TcpDxe/TcpMain.c:467 | EFI_TCP4_PROTOCOL.Close entry |
| `SockClose`       | NetworkPkg/TcpDxe/SockInterface.c:866 | Parks token in `Sock->CloseToken` |
| `TcpSetState`     | NetworkPkg/TcpDxe/TcpMisc.c:798 | Logs every state transition with `-DDEBUG_NET` |
| `SockConnClosed`  | NetworkPkg/TcpDxe/SockImpl.c:1011 | Calls `SIGNAL_TOKEN(CloseToken, EFI_SUCCESS)` |
| `SIGNAL_TOKEN`    | NetworkPkg/TcpDxe/SockImpl.h:23 | Macro: `Token->Status=...; gBS->SignalEvent(Token->Event)` |

---

## Worked example: tracing a TCP close

Set breakpoints to trace the entire close path with one curl
request. Each breakpoint prints state and continues, so the test
still runs at near-normal speed:

```
b axl_tcp_close
  commands
    silent
    printf "axl_tcp_close sock=%p tcp4=%p\n", sock, sock->tcp4
    cont
  end

b TcpSetState
  commands
    silent
    printf "TcpSetState Tcb=%p  state %d -> %d  CloseToken=%p\n", \
           Tcb, Tcb->State, State, Tcb->Sk->CloseToken
    cont
  end

b SockConnClosed
  commands
    silent
    printf "SockConnClosed Sk=%p CloseToken=%p Event=%p\n", \
           Sock, Sock->CloseToken, \
           Sock->CloseToken \
             ? ((SOCK_COMPLETION_TOKEN *)Sock->CloseToken)->Event : 0
    cont
  end

c
```

Then trigger one curl from the host. The output bisects the four
likely TCP-close failure modes:

- `axl_tcp_close` fires but no `TcpSetState` follows: driver isn't
  receiving the FIN-ACK (MNP/Ip4 not pumping).
- `TcpSetState` runs but never reaches `TCP_CLOSED` (state 7 in
  `mTcpStateName`): peer didn't reply, or our FIN was queued
  unflushed.
- `TCP_CLOSED` reached AND `SockConnClosed` fires but our wait
  still times out: `Sock->CloseToken` cleared via a race, or the
  Event handle differs from the one we passed in.
- `SockConnClosed` fires with the right Event but our wait still
  times out: someone closed the event between our `gBS->CreateEvent`
  and `Tcp4Close`. Add `b CoreSignalEvent if UserEvent ==
  <our_handle>` to confirm.

---

## Limitations and gotchas

- **No KVM under `--gdb`.** TCG is ~5-10x slower; budget extra time
  for boot. Tests that pass under KVM may time out under TCG.
- **Watchpoints are slow.** Hardware watchpoints work but each
  memory access traps. Use sparingly; prefer source breakpoints.
- **Symbol load is one-shot.** If new modules load AFTER you
  `source /tmp/syms.gdb`, regenerate the script and `source` it
  again. The dispatcher loads most DXE modules in one batch
  pre-shell, so this rarely bites.
- **AXL EFI symbols depend on the matching build.** `gdb-syms.py`
  reads the `.so` next to the `.efi` for the `.text` offset; if
  you `make tests` between attaching and re-attaching, regenerate.
- **Stripped EDK2 modules.** A handful of modules (notably
  `CpuDxe_<GUID>.debug`) ship with a GUID-suffixed filename. The
  script handles this with a `<NAME>_*.debug` glob fallback;
  watch for any "missing .debug for: ..." comments at the end of
  the generated script.

---

## See also

- `BISECT-PLAN.md` (root, working file) — current open
  investigation: TCP close-event hangs in test-http.sh.
- [src/event/README.md](../src/event/README.md) — AxlCancellable /
  AxlWait / AxlEvent design.
- [docs/AXL-Concurrency.md](AXL-Concurrency.md) — TPL contracts and
  the loop's idle path.
