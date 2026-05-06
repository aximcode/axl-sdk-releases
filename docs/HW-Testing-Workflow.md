# Hardware Testing Workflow — axl-webfs PUT loop

When the slow path is "rebuild uefi-devkit virtual media → respin →
reattach → reboot," you can hit a much faster iteration loop by using
**axl-sdk's own `axl-webfs` to PUT updated binaries directly into a
RAM disk on the running host**. Build → test cycle drops from
minutes to seconds.

This document captures the pattern that worked on Dell PowerEdge
XE7745 (iDRAC10) on 2026-05-06.

---

## When this works

- Host is up at the UEFI shell with a usable network interface.
- Host's network is reachable from your laptop (or wherever curl will
  run from).
- A previous build is already deployed (the running `axl-webfs.efi`
  serves files; updated binaries replace the on-disk RAM-disk copies).

If any of those don't hold, fall back to a virtual-media respin.

---

## One-time setup per boot

```nsh
# On host UEFI shell — create a writable RAM disk for staged uploads.
fs4:\x64\MkRd.efi uploads -s 8
map -r
# RAM disk shows up as fs5:
```

If `MkRd` complains it has no `RamDiskDxe.efi`, see
`src/util/axl-driver.c` — it ships the EDK2 binary embedded.

If the host has a USB-RNDIS NIC (iDRAC virtual NIC), apply the
packet-filter workaround once per boot:

```nsh
fs4:\x64\RndisFix.efi
ifconfig -s eth1 static 169.254.1.2 255.255.255.0 169.254.1.1
```

---

## Iteration loop (build → push → run)

### 1. Build locally

```bash
cd ~/projects/aximcode/axl-sdk
make tools ARCH=x64           # rebuild whichever tools changed
```

### 2. Start axl-webfs serve on the host

The `laptop console` wrapper connects to the iDRAC and pipes
serial. Drive it with the `slowtype` helper below — the EDK2 shell
drops characters at native pipe speed (~14 char/s is the safe rate
through racadm `console com2`):

```bash
slowtype() {
    local s="$1"; local i
    for ((i=0; i<${#s}; i++)); do
        printf '%s' "${s:$i:1}"
        sleep 0.07
    done
    printf '\r'
}

{
    sleep 5
    printf '\r'
    sleep 2
    slowtype 'fs4:'
    sleep 1
    slowtype 'x64\axl-webfs.efi serve -p 8080'
    sleep 60                       # serve runs for the curl window
    printf '\x1b'                   # ESC to stop the serve loop
    sleep 4
    printf '\x1c'                   # Ctrl-\ to exit racadm console
    sleep 1
} | timeout 90 laptop console 10.215.120.97 2>&1 \
  | sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x0f//g; s/\x1b\([A-Z]//g; s/\x00//g' \
  > /tmp/host-serve.log &
WEBFS_PID=$!

sleep 10                            # wait for serve to bind
```

### 3. Push updated binaries from your laptop

ssh-side note: `ashley → laptop-tunnel (port 2223) → laptop WSL`,
and the laptop is the host that has direct IP reachability to the
host's eth0 (`10.9.177.98` in our session). curl runs from there.

```bash
# Copy the rebuilt binaries from ashley to the laptop tmp dir.
scp -q out/native-x64/tools/{Foo,Bar}.efi laptop-tunnel:/tmp/

ssh laptop-tunnel "
  curl -s -m 10 -T /tmp/Foo.efi -w 'HTTP %{http_code}\n' \\
    http://10.9.177.98:8080/fs5/Foo.efi
  curl -s -m 10 -T /tmp/Bar.efi -w 'HTTP %{http_code}\n' \\
    http://10.9.177.98:8080/fs5/Bar.efi
"

wait $WEBFS_PID 2>/dev/null         # let serve finish + exit cleanly
```

### 4. Run the new binary on the host

```bash
{
    sleep 5
    printf '\r'
    sleep 2
    slowtype 'fs5:\Foo.efi --whatever-args'
    sleep 15
    printf '\x1c'
    sleep 1
} | timeout 30 laptop console 10.215.120.97 2>&1 \
  | sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x0f//g; s/\x1b\([A-Z]//g; s/\x00//g' \
  > /tmp/test-output.log

tail -30 /tmp/test-output.log
```

The iteration cycle is bounded by the `sleep 60` in step 2 and
network round-trips — typically 30s end-to-end vs minutes for a
respin.

---

## Verified-working test patterns

### File transfer roundtrip (axl-webfs serve itself)

```bash
# From the laptop:
echo "test payload $(date +%s)" > /tmp/test.txt
curl -T /tmp/test.txt -w 'PUT %{http_code}\n' http://10.9.177.98:8080/fs5/test.txt
curl http://10.9.177.98:8080/fs5/test.txt
curl -X DELETE -w 'DELETE %{http_code}\n' http://10.9.177.98:8080/fs5/test.txt
curl -o /dev/null -w '404 expected: %{http_code}\n' http://10.9.177.98:8080/fs5/test.txt

# 100KB binary roundtrip:
dd if=/dev/urandom of=/tmp/blob.bin bs=1024 count=100 2>/dev/null
curl -T /tmp/blob.bin http://10.9.177.98:8080/fs5/blob.bin
curl -o /tmp/blob-back.bin http://10.9.177.98:8080/fs5/blob.bin
md5sum /tmp/blob.bin /tmp/blob-back.bin   # must match
```

### Range request

```bash
curl -H 'Range: bytes=0-15' http://10.9.177.98:8080/fs5/some-binary | xxd | head -2
```

### mkdir / list

```bash
curl -X POST -w '%{http_code}\n' 'http://10.9.177.98:8080/fs5/subdir/?mkdir'
curl -H 'Accept: application/json' http://10.9.177.98:8080/fs5/
```

---

## Limits

- **Read-only volumes** (virtual CD = `fs4`) can be browsed via GET
  but obviously not PUT'd. Use the RAM disk.
- **NVMe / OS disks** (`fs0`, `fs1`) are mounted read-write in UEFI
  but writing to them is risky — don't.
- **The serve session is foreground.** While `axl-webfs.efi serve`
  is running you can't drive other host commands until it exits.
  Time the curl window with the host-side `sleep 60` accordingly.
- **Multi-NIC selection** — if the host has eth0 and eth1 both
  configured, axl-webfs `serve` auto-picks first valid handle (which
  is firmware-enumeration-order-dependent). Use
  `axl-webfs.efi serve -p 8080 --source <ip>` to pin explicitly.
- **iDRAC USB-NIC data plane needs `RndisFix.efi` once per boot.**
  See `tools/rndisfix.c` for the EDK2 stub workaround.

---

## Lab connectivity (the SSH chain)

The dev box (`ashley`) doesn't have IP reachability to the lab
host directly. Traffic flows through a laptop on the corp/lab
network:

```
ashley ──reverse-tunnel:2223──> laptop-tunnel ──> laptop (WSL)
                                                    ├──> ssh idrac (sshpass)
                                                    │      └──> racadm "console com2" → host UEFI shell over BIOS-redirected serial
                                                    └──> curl http://10.9.177.98:8080/... (when host axl-webfs serve is up)
```

Single-hop ssh from ashley needs this in `~/.ssh/config`:

```
Host laptop-tunnel
    HostName localhost
    Port 2223
    User <your-user>

Host idrac-* 10.9.177.* 10.9.176.* 10.215.*
    User root
    ProxyJump laptop-tunnel
    StrictHostKeyChecking accept-new
    UserKnownHostsFile ~/.ssh/known_hosts
```

`sshpass` is needed on both ashley and laptop. The `laptop`
wrapper script (in `~/bin/laptop`) bundles the
`ssh ... idrac ... racadm console com2` chain into one command —
adapt the wrapper to your environment if you set this up fresh.
At minimum it needs subcommands for:

- `idrac [host] [args]` — racadm one-shot or interactive
- `console [host]` — racadm `console com2` for serial-redirected
  UEFI shell (used by every `slowtype | laptop console …` block
  in this doc)
- `sol [host]` — IPMI Serial-over-LAN (iDRAC10 may fail with
  UDP/623 RMCP+ session error; use `console` instead)

## Linux-side investigation

When something doesn't work in axl-sdk on real hardware but
Linux's tools do (e.g., `ipmitool` reads BMC fine but our
`ipmi.efi` can't), boot the host into Linux and capture
ground-truth data from there. The four-bug iDRAC10 KCS state
machine fix (commit 6088d47) was traced this way — Linux's
`ipmi_si` driver works on the same hardware where ours didn't,
so the bug was in our code, not the BMC.

Reboot the host into Linux (any modern distro live USB/ISO
works — the `casper`/`live` LABEL=ESP boot params in the captured
DSDT log show what we used). Then:

```bash
# From ashley, ssh through the tunnel to the host's Linux
ssh laptop-tunnel "ssh <linux-IP>"

# Or run a probe script via:
scp -q my-probe.sh laptop-tunnel:/tmp/ && \
ssh laptop-tunnel "scp /tmp/my-probe.sh <linux-IP>:/tmp/ && \
                   ssh <linux-IP> 'bash /tmp/my-probe.sh'" \
    > local-capture.log
```

Useful one-shot capture commands:

```bash
# IPMI ground truth
sudo dmesg | grep -iE 'ipmi|kcs|bmc'   # which I/O ports + spacing the kernel uses
sudo cat /proc/ioports | grep ipmi      # ditto
sudo ipmitool mc info                   # canonical Get Device ID body
sudo ipmitool raw 6 1                   # raw response bytes
echo 0xfff | sudo tee /sys/module/ipmi_si/parameters/kcs_debug
sudo dmesg -C && sudo ipmitool raw 6 1 >/dev/null && sudo dmesg | tail
                                        # KCS state-machine trace per byte

# I2C / SMBus inventory (incl. DIMM SPD bus)
sudo apt-get install -y i2c-tools
sudo i2cdetect -l                       # all i2c buses
for n in 0 1 2 3; do sudo i2cdetect -y -r $n 0x50 0x57; done
                                        # which bus carries DIMM SPDs

# SMBIOS / DMI (compare against sysinfo / dmidecode tools)
sudo dmidecode -t 38                    # IPMI Type 38 incl. spacing decode
sudo dmidecode -t 17                    # per-DIMM details
sudo cp /sys/firmware/dmi/tables/DMI /tmp/DMI && sudo chmod 644 /tmp/DMI
                                        # binary blob; replay with
                                        # dmidecode --from-dump=DMI

# ACPI
ls /sys/firmware/acpi/tables/
sudo cp /sys/firmware/acpi/tables/DSDT /tmp/dsdt.aml
sudo apt-get install -y acpica-tools
(cd /tmp && sudo iasl -d dsdt.aml)      # DSDT.dsl is human-readable
                                        # grep for 'IPI0001' (IPMI device),
                                        # 'NIPM' (Dell ACPI IPMI ops region)

# PCI / USB
lspci -nn                               # full
lspci -tv                               # tree
sudo lspci -vvv -s 00:14.0              # PCH SMBus deep dive
lsusb -t                                # tree
sudo lsusb -v -d 0bda:8153              # specific device descriptors
```

Save anything potentially useful into `DellXE7745/` (or whatever
named directory for the platform you're investigating); see the
`DellXE7745/README.md` index for what we captured for the iDRAC10
investigation. The directory is gitignored — captures aren't
distributable, but they're invaluable for the next investigator
of the same platform.

## Troubleshooting

| symptom | likely cause | fix |
|---|---|---|
| `slowtype` losing trailing characters | racadm console too slow | bump per-char `sleep` to 0.10 (default 0.07) |
| `serve probe HTTP 000` after sleep 14 | axl-webfs not yet bound | extend the sleep to 18-20s; the EDK2 shell + driver-load takes a moment |
| `MkRd: failed to register EFI_RAM_DISK_PROTOCOL` | RamDiskDxe not embedded | check `tools/mkrd-blob.S` was rebuilt; `make tools` after a clean tree |
| `ipmi.efi --transport kcs` fails on a Dell box | Type 38 says one spacing, hardware uses another | `mm 0xCA8/CA9/CAC/CB8 -IO -w 1 -n` from UEFI shell to find the live ports; check `dmesg | grep ipmi` from a Linux boot for ground truth |
| `axl-webfs serve` doesn't see eth1 | RndisFix not applied this boot | re-run `RndisFix.efi` + `ifconfig -s eth1 static …` (per-boot setup section above) |
| `ipmi info` returns garbage but raw works on iDRAC10-class | typed wrapper buffer too small for echo + body + trailing vendor bytes | already fixed (kcs_send_raw stages internally) — file a bug if it recurs |
| host wedges with random Load Error after axl-sdk tools | known issue, not dismissed; capture ritual in ROADMAP "Real-hardware findings — Load Error" | reboot host; capture stress-pair if reproducible |

## Why this exists

Real-hardware bugs surfaced during the XE7745 in-band Redfish work
(2026-05-04 → 2026-05-06) — chunked decode, TLS staging, RNDIS
packet filter, multi-NIC routing, microsecond logging, FtVolume
layout drift, axl-webfs teardown, four-bug iDRAC10 KCS state
machine. Many required iteration on the real platform. Each respin
was 5+ minutes; this loop is 30s.

The Linux-side investigation pattern saved at least a day of
guess-and-test on the KCS work — `dmesg | grep ipmi` told us the
exact ports + spacing in seconds, vs. probing every plausible
combination from UEFI shell.
