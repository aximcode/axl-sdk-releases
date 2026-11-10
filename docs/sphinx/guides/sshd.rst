The sshd Tool
=============

``sshd`` serves the live UEFI Shell over SSH. It deploys a resident driver
that takes over the console -- the local display keeps painting -- and
returns to the Shell at once. Every ``ssh -t`` session then sees and types
into that same Shell, as if at the keyboard; a session that joins late is
repainted with the current screen.

Starting the server
-------------------

Put an ``authorized_keys`` file (``ssh-ed25519`` lines) beside ``sshd.efi``
-- in the same directory, wherever that is on its volume -- and run it::

    FS0:\> sshd
    sshd: starting -- bringing up the network (up to 60 s)
    sshd: no FS0:\tools\sshd.conf; using defaults
    sshd: serving the UEFI Shell on 192.0.2.10 port 22
    sshd: generated a new host key
    sshd: host key ED25519 key fingerprint is SHA256:q+Gvo2Uyo27fLUsWIEE6XoTiE6UAh3uj4r83wNtSbRE.

The last two lines are the host key (see `The host key`_): ``generated`` only
at the very first start on a machine, the fingerprint at every start, in the
words ``ssh`` uses when it first meets the server, so the two can be compared
character for character.

The first line comes before anything else is done, so a start that takes a
while is never mistaken for one that printed nothing (see `Starting up`_).
The second names the ``sshd.conf`` it looked for (``sshd: settings from
FS0:\tools\sshd.conf`` when it read one), so a file put anywhere else is
seen not to be read. The last names where to connect: the server listens
on every address, as OpenSSH's default ``0.0.0.0`` does, and the line lists
each address a NIC holds, comma-separated
(``on 192.0.2.10, 198.51.100.7 port 22``).

``sshd status`` says whether it is running -- ``sshd: running on
192.0.2.10`` -- ``sshd log`` prints its log, and ``sshd stop`` unloads it.
The addresses are read from the firmware's NIC list when the line is
printed, so they include one
another tool set, ``0.0.0.0`` stands for none, and ``status`` gives no
port: it cannot see the running server's. More addresses than fit are
counted, as ``+N more``. A
second ``sshd`` while one is running changes nothing and says so; stop it
first to change its settings.

The flags:

==============================  =================================================
``-p``, ``--port N``            TCP port (default 22)
``-n``, ``--nic N``             NIC ordinal to bring up (default: the first usable)
``-k``, ``--keys PATH``         ``authorized_keys`` file
``-P``, ``--password PW``       also accept this password, for any user name
``-R``, ``--read-only``         sessions watch the console; their input is dropped,
                                and SFTP refuses every write
``--sftp-read-only``            SFTP refuses every write; reads still work
``--report-geometry``           write each console reshape to the first serial port
``--log-port PORT``             serial port the server's log goes to while it
                                serves: ``auto`` (default), ``none``, or a
                                port number, 0 for the first
``--net-timeout N``             seconds the network bring-up may take
                                (1-86400; default 60)
``--tcp-linger US``             microseconds a connection stays polled after a
                                reply (0-20000; default 0, off); see
                                `Request/response latency: AxlTcpLinger`_
``-x``, ``--exempt CIDR``       never penalise failed logins from this range
                                (repeatable, up to 8)
``-f``, ``--config-file PATH``  settings file (default: ``sshd.conf`` beside
                                ``sshd.efi``)
``-T``, ``--dump-config``       print the effective settings and exit
``-v``, ``--verbose``           log at DEBUG (``LogLevel DEBUG``), naming each
                                step of the start before it runs
``-d``, ``--debug``             the same as ``-v`` (OpenSSH sshd's ``-d``); the
                                server still returns to the Shell
==============================  =================================================

A path with no volume (``keys\ak``, ``my.conf``) is relative to the
directory ``sshd.efi`` is in, not the Shell's current directory; one that
starts with ``\`` (``\keys\ak``) is from the root of ``sshd.efi``'s volume.
``-k ""`` means no keys file at
all -- not the default ``authorized_keys``, nor an ``AuthorizedKeysFile``
from ``sshd.conf`` -- and ``-P ""`` means no password, clearing an
``AxlPassword`` from ``sshd.conf``. Neither turns its method on (``sshd
-T`` prints ``authorizedkeysfile none`` or ``axlpassword none``); with
nothing else to log in by, ``sshd`` refuses to start.

Starting up
-----------

Before it returns, ``sshd`` brings the network up -- it is the server's,
started by its driver while the ``sshd`` command waits -- then takes over
the console, builds the server and listens. ``sshd.efi`` alone is enough:
it brings a box online the way ``netload -a`` does, with the same engine
(``axl_net_auto_init_opts``), so ``netload`` need not run first. For a NIC
the firmware has no driver for -- a USB NIC dongle, say -- put the driver
in ``drivers\<arch>`` beside ``sshd.efi`` (``drivers\x64`` or
``drivers\aa64``; the release's tree already has that directory). The
network is the part that can take time, so ``sshd`` says it is starting,
with the deadline, before anything else, and the bring-up is bounded:

- **A NIC already has an address** (``ifconfig``, ``netload``): it is used
  as it is, and nothing else below runs.
- **Old shells see it too.** Once the network is up, the EFI 1.x shell's
  ``ifconfig -l`` shows the NIC's address, mask and gateway rather than
  ``Not configured.``: AXL publishes the pre-UEFI-2.4 IPv4 configuration
  protocols that shell reads, read-only, from a small resident driver (see
  the network module's "Old tools" section). ``ifconfig -s`` there is
  refused; configure with ``netload`` or the modern Shell's ``ifconfig``.
- **The firmware's NICs first.** ``sshd`` connects the firmware's own NIC
  drivers and waits up to 5 s for a link.
- **Link-down NICs are skipped**, read as ``netload`` reads them: a NIC
  with no cable cannot lease, so it gets no DHCP.
- **Then the drivers beside sshd.efi**, swept as ``netload -a`` sweeps
  them: ``drivers\<arch>`` beside ``sshd.efi``, else ``drivers``, else
  ``\drivers\<arch>`` at its volume's root; when none exists, every
  volume's ``\drivers\<arch>``. Each driver is loaded in turn, a NIC it
  brings gets the same link wait and DHCP, and the sweep stops at the
  first that comes up; one that brings nothing up is unloaded. The sweep
  is crash-safe and shares ``netload``'s quarantine: a driver that hung or
  reset the box on an earlier try is skipped (``netload --clear`` retries
  it), and so is one ``netload`` quarantined. A ``netload-drivers.json5``
  in the directory names companion drivers (NetworkCommon for USB
  CDC/RNDIS), which are loaded first and never swept on their own.
- **DHCP** runs on every link-up NIC of a step AT ONCE -- as Linux's
  ``ip=dhcp``, systemd-networkd and dhcpcd do -- for up to 15 s between them
  (what ``netload`` gives a NIC); the first NIC to lease is used, and every
  other NIC's DHCP is stopped and the NIC put back as it was -- its policy
  and, for a NIC set static, its address, gateway and DNS servers (``ifconfig
  -l`` shows them). Each step therefore costs the link wait and one DHCP wait
  however many ports are cabled, so onboard ports on a network with no DHCP
  server do not keep the sweep from the driver of the NIC that would
  lease.
- **One deadline** bounds all of it -- the firmware's NICs, the sweep,
  every link wait and DHCP: ``sshd: network bring-up timed out after 60
  s``. It is 60 s unless ``AxlNetworkTimeout`` in ``sshd.conf`` or
  ``--net-timeout`` says otherwise (1 s to one day), and the starting line
  names it. 60 s leaves room for the firmware pass's link wait and DHCP
  (5 + 15 s), the winning driver's (5 + 15 s), and 20 s of loading and
  connecting drivers; no driver is loaded once it has passed. A box with a
  slow USB NIC, or more cabled ports than that allows for, needs a longer
  one.
- **Ctrl-C** stops the bring-up at once -- a wait, or the sweep before its
  next driver: ``sshd: network bring-up stopped (Ctrl-C)``.
- **No network.** With no NIC at all it stops after the link wait -- a USB
  NIC still enumerating, or the NIC ``-n`` names, may appear during it --
  naming what it searched: ``sshd: no network: no NIC driver produced an interface
  (searched the firmware and FS0:\tools\drivers\x64)`` (or ``\drivers\x64
  on every volume`` when there is no drivers directory). With every NIC it
  found link-down it says, after the link wait, ``sshd: no network: no NIC
  has a link (4 link down)``; with ``-n K``, ``sshd: no network: NIC K
  (<mac>) has no link``. When NICs had a link but none leased: ``sshd: no
  network: no DHCP lease on any NIC with a link (2 tried)``.
- **Options too long.** The options travel to the server in its
  LoadOptions, 4,096 bytes URL-encoded; a longer set (a very long password
  or path) is refused with ``service 'sshd': its options (URL-encoded) do
  not fit the 4096-byte LoadOptions -- shorten a long path or string
  option``.

On any of these ``sshd`` adds ``sshd: could not start the server`` and
exits with an error, with nothing left installed: the console is not taken
over until the network is up, a driver the sweep loaded that brought no NIC
up is unloaded, so are the companions the sweep loaded (the winner keeps
those it needs), and a DHCP that ``sshd`` started and then gave up on is
stopped -- the NIC's policy is put back as it was (``ifconfig -l`` shows
it).

``-v`` (or ``-d``, or ``LogLevel DEBUG``) names each step at DEBUG before
it runs -- a DEBUG line carries its function and line number, NNNN below --
in the server's own log, which goes to the console until the
server is serving::

    [DEBUG] sshd:net_up:NNNN: bringing up the network (NIC auto, up to 60 s)
    [DEBUG] sshd:net_up:NNNN: drivers directory: FS0:\tools\drivers\x64
    [DEBUG] sshd:net_progress:NNNN: firmware pass: connecting the firmware's NIC drivers
    [DEBUG] sshd:net_progress:NNNN: NIC 02:00:00:00:00:01 link down -- skipping
    [DEBUG] sshd:net_progress:NNNN: firmware pass: no NIC with a link
    [DEBUG] sshd:net_progress:NNNN: trying driver RtkUsbUndiDxe.efi
    [DEBUG] sshd:net_progress:NNNN: DHCP on NIC 02:00:00:00:00:02 (up to 15 s)
    [DEBUG] sshd:net_progress:NNNN: driver RtkUsbUndiDxe.efi brought NIC 02:00:00:00:00:02 up with 192.0.2.10
    [DEBUG] sshd:sshd_setup:NNNN: taking over the console
    [DEBUG] sshd:sshd_setup:NNNN: building the server (host key, authorized keys)
    [DEBUG] sshd:sshd_setup:NNNN: listening on port 22
    [DEBUG] sshd:log_port_open:NNNN: the log is not written while serving: every serial port carries the console

The sweep's other lines are ``driver X: no NIC with a link -- unloaded``,
``driver X failed to load``, ``skipping driver X -- quarantined (...)`` and
``loaded companion driver X``. So a start that stalls says where. The
deadline and Ctrl-C bound every *wait*; a firmware call that itself never
returns -- loading or connecting a NIC driver (each is named before it
runs), a DHCP start on firmware with no ``IP4Config2`` -- cannot be cut
short by any UEFI program, and the line before it is what names it. A
driver that hangs the box that way is quarantined on the next try. ``set
AXL_LOG_LEVEL debug`` in the Shell shows the same lines, and the network
library's own.

sshd-rtk.efi
------------

``sshd-rtk.efi`` is ``sshd.efi`` with Realtek's USB NIC driver,
``RtkUsbUndiDxe.efi``, inside it, for a box whose cabled NIC is a Realtek
USB dongle the firmware has no driver for: copy the one file and run it, no
``drivers\<arch>`` needed. The embedded driver is the bring-up's first
candidate after the firmware's own NICs -- before anything in
``drivers\<arch>`` -- loaded from memory under the same crash guard,
quarantine, deadline and Ctrl-C, and ``-v`` names it::

    [DEBUG] sshd:net_progress:NNNN: trying embedded driver RtkUsbUndiDxe.efi

Everything else is ``sshd``'s: the flags, ``sshd.conf`` beside it, the
messages (they still start ``sshd:``). The two are ONE service: ``sshd
status`` and ``sshd stop`` see and stop a server ``sshd-rtk`` started, and
the other way round. Stopping it leaves the NIC up -- the driver and its
lease stay -- so a later ``sshd`` uses that address without a sweep.

The driver is Realtek's binary and is not in this tree, so ``sshd-rtk.efi``
is built only when given it, one arch at a time::

    scripts/axl-build.sh ARCH=x64 sshd-rtk RTK_USB_UNDI=/path/x64/RtkUsbUndiDxe.efi

(packed with ``axl-efi-pack`` before it is embedded; the image lands in a
build tree of its own, ``build/x64-debug-rtk/tools/sshd-rtk.efi``. The
``sshd-rtk`` goal without ``RTK_USB_UNDI`` builds nothing and says so). A release or test
release carries it only when asked: ``make-release-assets.sh uefi-tools
--with-rtk DIR`` / ``make-test-release.sh --with-rtk DIR``, with
``DIR/<arch>/RtkUsbUndiDxe.efi``, and the README and release notes say it
embeds Realtek's driver.

Copying files
-------------

``sftp`` and ``scp`` (which speaks SFTP since OpenSSH 9.0) copy files to
and from the machine's volumes over the same server, with the keys or
password the Shell takes::

    $ sftp -P 22 root@host
    sftp> ls /
    /fs0  /fs1
    sftp> get /fs0/logs/boot.log
    sftp> put mytool.efi /fs0/tools/mytool.efi

    $ scp root@host:/fs0/tools/report.txt .
    $ scp mytool.efi root@host:/fs0/tools/mytool.efi

**Paths.** ``/`` lists the mapped volumes, lower-case; ``/fs0/dir/file``
names a file on ``FS0:``, and replies always use that form. The Shell's own
form names the same file: ``fs0:/dir/file`` and ``FS0:\dir\file`` are
accepted by the server, and the volume's case does not matter. But the
clients rewrite a path before the server sees it, so the Shell form needs
care in both.

``sftp`` reads its command line as a shell would: an unquoted ``\`` is an
escape, and ``get`` also runs the remote path through ``glob(3)``, so a
single ``\`` is lost even in quotes -- ``get FS0:\tools\report.txt`` asks
for ``FS0:toolsreport.txt``. And a path that does not start with ``/`` is
joined to the remote working directory, so ``FS0:/tools/report.txt`` names
that file only while the working directory is ``/`` (where a session
starts). In ``sftp``, use ``/fs0/tools/report.txt`` anywhere; the Shell form
only from ``/``, with ``/`` or each ``\`` doubled::

    sftp> get /fs0/tools/report.txt
    sftp> cd /
    sftp> get FS0:/tools/report.txt
    sftp> get FS0:\\tools\\report.txt

``scp`` splits ``host:path`` at the first colon, which is fine, but then
runs the remote path through ``glob(3)`` too -- ``host:'FS0:\f'`` arrives as
``FS0:f`` -- and resolves a relative path against the login directory,
which is ``/``. So with ``scp`` write the Shell form with ``/`` or a doubled
``\\``::

    $ scp root@host:'FS0:/tools/report.txt' .
    $ scp mytool.efi root@host:'FS0:\\tools\\mytool.efi'

**Writing.** ``put``, ``rm``, ``mkdir``, ``rmdir`` and ``rename`` work as
against OpenSSH's own server, with its error for each refusal: a file with
the read-only attribute shows as mode ``0444`` and cannot be overwritten,
removed or renamed. ``/`` is not yours to change -- nothing can be made in
it, and a volume cannot be removed or renamed. ``rename`` replaces an
existing file, as ``rename(2)`` does (``sftp`` sends
``posix-rename@openssh.com``, which the server advertises, and the server
hands it to AXL's ``axl_file_replace``); a directory replaces only an empty
directory, a file never replaces a directory, and a file with the
read-only attribute is never replaced. FAT cannot rename over a name, so
the old file is first renamed aside (to ``~AXLhhhh.TMP`` in its
directory), the new one renamed into place, and only then is the aside
removed; a failure AXL sees puts both back. It is not atomic, though: the
UEFI FAT driver can itself lose an entry under both names when a
directory it must grow runs out of space (AXL refuses a replace when the
volume reports too little room, but a FAT32 volume's figure can be
stale), and after a power loss in the middle the old file is left at its
``~AXLhhhh.TMP`` name. ``rename -l`` (the original SFTP rename) never
replaces. A rename moves an entry between directories of one volume in a
single step, without copying; between volumes it is refused (*Failure*),
so copy and remove instead.

A name and its 8.3 alias -- ``longfilename.txt`` and ``LONGFI~1.TXT`` --
are one file to FAT, so ``rename`` of one onto the other succeeds and
changes nothing, as ``rename(2)`` of a file onto itself does. A
``rename`` that changes only letter case (``b.txt`` to ``B.TXT``) renames
the file to that spelling. ``rename -l`` refuses both (*Failure*): the
name exists.

``put -p`` and ``scp -p`` keep a file's modification time. FAT stores it
in two-second steps: an odd second lands one second lower. FAT keeps no
time of day for the access time, so it is not set.

**A transfer is paced by the server, not only by the network.** The SFTP
session reports each request as it starts to answer it, and only then
reopens the client's SSH window -- as OpenSSH's ``sshd`` reopens it only as
``sftp-server`` reads its pipe. So while a large ``put`` waits on the disk,
or on a reply the client is slow to read, the client stops after one window
(2 MB) instead of the server buffering everything it sends.

Where this differs from OpenSSH's ``sftp-server`` and ``sshd``:

* A client that sends past its SSH window is disconnected once the server
  holds the window plus 10% of unanswered input, even when it interleaves
  empty data packets. ``sshd`` allows the same 10%, but an empty packet
  resets its count, so such a client is never stopped there. No client
  that honours the window is affected.

* A time before 1981 is stored as 1981-01-01 00:00:00 UTC and the request
  succeeds, so ``put -p`` of a file stamped with the epoch (for example
  ``SOURCE_DATE_EPOCH=0``) works. A Linux FAT mount clamps to 1980-01-01;
  the UEFI FAT driver cannot store any 1980 time, so the clamp lands on the
  first second of 1981.
* Setting a time through a file opened only for reading is refused with
  *Permission denied*; ``sftp-server`` allows it. ``put -p`` and
  ``scp -p`` set times through the handle they wrote with, so they are not
  affected.
* Setting a time on a volume root (``/fs0``) succeeds and changes nothing:
  FAT keeps no time for its root directory.
* A ``rename`` on a write-protected volume is refused with *Permission
  denied*; ``sftp-server`` answers *Failure* there (``EROFS``). AXL reports
  a write-protected volume as it reports a read-only entry.
* One request may grow a file by at most 16 MiB past its end -- a size set
  with ``SETSTAT``/``FSETSTAT``, or the gap a write leaves before its data;
  more is refused (*Failure*) and the size is unchanged (a time set in the
  same request is still applied, as ``sftp-server`` applies each attribute
  in turn). The UEFI FAT
  driver writes zeros over every added byte inside the one call, and sshd
  serves every session from one loop, so a large growth would freeze every
  console session until it finished: 16 MiB is about 60 ms under QEMU and
  up to about 1.6 s on slow USB flash. ``sftp``, ``scp`` and a resumed
  ``reput`` write in order and never leave a gap.

**Limits.** The server reports them to a client that asks
(``limits@openssh.com``, which ``sftp`` uses to size its requests): packets
up to 256 KB, up to 32 KB of data per read, up to 255 KB per write, and 16
files or directories open at once per session.

**Speed.** Measured under QEMU with KVM on x64 (DEBUG build, ``sftp``'s
default of 64 requests in flight): about 7 MB/s for ``get`` and 2 MB/s for
``put``. A client that keeps only one request outstanding (``sftp -R 1``)
gets about 1.5 MB/s, because each round trip waits on the server.

``AxlSftpReadOnly yes`` (or ``--sftp-read-only``) keeps SFTP on but
refuses every write with *Permission denied*, as ``sftp-server -R`` does;
``AxlReadOnly yes`` (``-R``) implies it, since a session that cannot type
should not be able to replace the tools it watches run::

    sftp> put mytool.efi /fs0/tools/mytool.efi
    dest open "/fs0/tools/mytool.efi": Permission denied

Like ``sftp-server -R``, it then does not advertise ``posix-rename``, so
``sftp``'s ``rename`` sends the original request, which is refused the same
way.

When it is ``AxlReadOnly`` (or ``-R``) alone that makes SFTP read-only --
``AxlSftpReadOnly`` is ``no`` or absent and ``--sftp-read-only`` was not
given -- sshd's log says so once, when it starts serving::

    sftp: read-only (AxlReadOnly)

Nothing is logged when ``AxlSftpReadOnly`` or ``--sftp-read-only`` asked for
it.

The SFTP server is a separate image, ``SftpServer.efi``, as OpenSSH's is a
separate program: ``sshd`` loads it from beside ``sshd.efi`` as it starts
and unloads it as it stops, so replacing or adding ``SftpServer.efi`` takes
effect at the next ``sshd`` start (``sshd stop``, then ``sshd``). With SFTP on
-- the default -- the helper stays loaded for as long as sshd serves, whether
or not anyone uses it. (It is not loaded per session, as OpenSSH runs
sftp-server: a session runs in an event callback, and UEFI forbids
``LoadImage`` and ``StartImage`` at that TPL.) ``sshd-full.efi`` carries its own copy, so it needs no file beside it
(``sshd-full-rtk.efi`` is ``sshd-full`` with ``sshd-rtk``'s NIC driver). The
shipped ``sshd.conf`` turns it on with ``Subsystem sftp SftpServer.efi``;
commented out, SFTP is refused, as in OpenSSH. With no ``sshd.conf`` at all
it is on. **An sshd.conf from an earlier release has no Subsystem line, so
SFTP is off with it until you add one**; sshd's log says so when it starts::

    sftp: off (sshd.conf has no Subsystem line)

If the helper is missing when sshd starts, sshd's log says so then::

    sftp: SFTP helper FS0:\SftpServer.efi not found; sftp sessions are refused

and each client that asks for SFTP is told::

    sshd: subsystem sftp: FS0:\SftpServer.efi not found
    Connection closed

Request/response latency: ``AxlTcpLinger``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A client that waits for each reply before it sends its next request --
``sftp -R 1``, or any tool that copies one block at a time -- waits on the
firmware too. Once a reply is sent nothing polls the network stack, so
the next request sits in the NIC until the firmware's 10 ms timer, and
then until sshd's next 10 ms tick: about 10-20 ms per request, however
fast the link. ``AxlTcpLinger N`` in ``sshd.conf`` (or ``--tcp-linger
N``) keeps a connection polled for *N* microseconds after each reply, so
a request that arrives within that time is answered at once.

It is **off by default** (0) and is a per-platform tuning knob: its
effect depends on the NIC and its UEFI driver, so measure before setting
it. Measured on QEMU (x64, a 16 MiB ``sftp -R 1`` download):

* virtio-net, ``AxlTcpLinger 9000``: 1.5 MB/s off, 16.3 MB/s on;
  ``-R 64`` downloads also went from 13.7 to 17.4 MB/s, ``-R 1`` uploads
  from 12.3 to 17.2.
* aarch64 (emulated, virtio-net): ``-R 1`` downloads 3.2 to 4.1 MB/s;
  uploads about 3% slower.
* QEMU's e1000 (the iPXE driver): downloads went from 3.0 to 13.3 MB/s,
  but uploads got slower -- 0.9 MB/s at ``-R 1`` and 3.6 at ``-R 64``,
  against 5.5. An e1000 discards received frames whenever its ring has no
  free buffer -- QEMU models this on purpose, as the real controller does
  (it counts them as missed packets) -- and iPXE's driver posts only 8
  receive buffers. Each poll refills the ring and lets the emulator deliver
  its whole queued backlog at once, so more polling means more frames
  discarded; each loss then waits out a TCP retransmission timeout. A real
  NIC whose UEFI driver keeps a similarly small receive ring may behave the
  same way.

The cost is a busy CPU for up to *N* microseconds after every reply. An
idle connection stops polling within that time, so an idle server does
not spin. The setting is read when sshd starts; sshd has no reload, so
``sshd stop`` and then ``sshd`` (with the new flag, or after editing
``sshd.conf``) to change it. Values run from 0 to 20000 (20 ms); a few
milliseconds, enough for one round trip to the client, is the useful
range.

Running from BMC virtual media
------------------------------

On a server booted through BMC virtual media, a large single read of a file
can come back corrupt while reads of 64 KB or less are reliable. Measured on
real hardware: ``sshd.efi --version`` launched from the virtual media failed
7 times in 10 with ``axl-pack: packed image is corrupt``, and ran every time
from a local USB copy or a RAM disk.

The large read is the firmware's own: the Shell's ``LoadImage`` reads the
whole file at once, before any of ``sshd``'s code runs. ``sshd.efi`` is a
packed (compressed) image, so its payload's CRC catches the bad bytes, and
it then reads its own file again, in 64 KB reads, before giving up::

    axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)

and starts as usual. Everything AXL reads after that -- ``sshd.conf``,
``authorized_keys``, the host key, the NIC drivers in ``drivers\<arch>``
(read by AXL and handed to ``LoadImage`` as a buffer) -- is read in 64 KB
pieces too. An UNPACKED tool has no CRC, so the firmware's bad read runs as
code instead.

Until the media is fixed, the dependable workaround is not to run tools
from it: copy them to a local disk, or to a RAM disk first (the Shell's
``cp`` reads in small pieces)::

    FS0:\> mkrd SCRATCH -s 16
    FS0:\> cp -r FS0:\tools SCRATCH:\
    FS0:\> SCRATCH:\tools\sshd

The host key
------------

**Where it is kept.** With no ``HostKey`` line, sshd keeps its Ed25519 host
key in a UEFI variable on the machine, ``AxlSshHostKeyEd25519`` under the
vendor GUID ``ef4ea128-0f65-4399-9884-9b0de9f54e9e``, made at the first
start. It is non-volatile and *boot-services only*: after the operating
system boots it cannot read the key through UEFI variable services, nor
create, overwrite or delete it. The identity belongs to the machine, not to
the stick or virtual media sshd runs from. Two things it does not buy:

- the key is stored unencrypted in the platform's variable flash, which a
  privileged operating system can read directly on many platforms;
- clearing NVRAM -- "load defaults", many firmware updates -- removes it, and
  the next start makes a new key, which every client then sees as changed.

The Shell sees it as ``dmpstore AxlSshHostKeyEd25519 -guid
ef4ea128-0f65-4399-9884-9b0de9f54e9e``.

**A stored key that is present but unusable is never replaced on its own.**
sshd refuses to start, and says why and what to do::

    sshd: the stored host key is unusable (the stored key has the wrong size); not starting. Run 'sshd hostkey --regenerate' to replace it

That covers a variable of the wrong size or format, one whose private key
does not derive its public key, one the firmware cannot read, and any whose
attributes are not exactly non-volatile and boot-services -- above all one
with *runtime* access, which the operating system could have planted before
the first start. Replacing it silently would hide tampering and teach every
client to accept a changed key.

**If it cannot be stored** -- a full or write-protected variable store --
sshd still serves, with a key made for this start only, and says so::

    sshd: WARNING: the host key could not be saved (cannot write the variable); it changes at every start
    FS0:\> sshd status
    sshd: running on 192.0.2.10 (degraded: host key not saved; it changes at every start)

**sshd hostkey** prints the key -- the running server's, or the one a start
would use -- as the fingerprint ``ssh`` shows and the line ``known_hosts``
takes::

    FS0:\> sshd hostkey
    sshd: the stored host key (UEFI variable AxlSshHostKeyEd25519):
    ED25519 key fingerprint is SHA256:q+Gvo2Uyo27fLUsWIEE6XoTiE6UAh3uj4r83wNtSbRE.
    ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIS5lbPwenfrU6yjeIJrBhYSLh3KffnzG91nzu8RsdRw

Prefixed with ``[host]:port`` that last line is a ``known_hosts`` entry, so a
client can be provisioned before it ever connects. ``sshd hostkey`` never
creates a key.

``sshd hostkey --regenerate`` deletes the stored key (stop the server first);
the next start makes a new one. It prints the fingerprint it deleted, so a
client's mismatch report can be matched to it. A variable the firmware will
not let it delete -- an authenticated one, which only its signer can -- is
left as it was and reported with the two ways out: a ``HostKey`` file, or
clearing NVRAM in firmware setup.

**A provisioned key.** ``HostKey`` in ``sshd.conf`` (or ``--host-key``) names
a key file instead, as ``ssh-keygen -t ed25519 -N ''`` writes it, beside
``sshd.efi``. It is repeatable and the first that loads is used; each one
that does not is reported in sshd's own two lines and skipped, and with none
left sshd stops (``sshd: no hostkeys available -- exiting.``). ``HostKey
none`` lines are skipped, so a file with only those has no host keys
available, as in OpenSSH. A refused file is said twice -- by ``sshd`` as it
checks the list, then by the server as it loads it, with its timestamp::

    sshd: Unable to load host key "FS0:\tools\ssh_host_rsa_key": ssh-rsa host keys are not supported; only ssh-ed25519
    sshd: Unable to load host key: FS0:\tools\ssh_host_rsa_key
    12:00:01.204522 [ERROR] sshd: Unable to load host key "FS0:\tools\ssh_host_rsa_key": ssh-rsa host keys are not supported; only ssh-ed25519
    12:00:01.204588 [ERROR] sshd: Unable to load host key: FS0:\tools\ssh_host_rsa_key

``sshd`` itself has no Ed25519 implementation -- the server does -- so it
cannot tell whether a key file's private key derives its public key. A file
damaged there passes ``sshd``'s check and is refused by the server alone, in
the same two lines; the next file in the list is then used. For the same
reason ``sshd hostkey`` names *the first readable host key file*, which a
start can still skip.

Only unencrypted ``ssh-ed25519`` keys in OpenSSH's own format are read; a
passphrase-protected key, another key type and a PKCS#8 file are each refused
by name -- convert the last with ``ssh-keygen -p -N '' -f FILE``. ``sshd -T``
prints one ``hostkey`` line per file, or ``hostkey
uefi-variable:AxlSshHostKeyEd25519`` without one.

**Prefer importing a key to leaving it on the medium.** FAT has no
permissions: a key file on an ESP or a stick can be read by the operating
system, by every UEFI application and by whoever holds the medium -- and on
BMC virtual media, by whatever carries the image. ``sshd hostkey --import
FILE`` stores the file's key in the variable instead; ``--force`` replaces a
key already stored, and puts it back if the new one cannot be written. Start
``sshd`` once and confirm the fingerprint it prints before removing the file:
as above, only the server checks the key fully, and a file damaged in
transit -- BMC virtual media is known to corrupt large reads -- is stored as
it is and refused at the next start.

Deviations from OpenSSH's ``sshd``, each for a reason: ``--host-key``
*replaces* the file's ``HostKey`` lines where ``sshd -h`` adds to them (a
flag wins over the file, as everywhere else here); at most eight ``HostKey``
lines; no ``PATH.pub`` companion check and no unprotected-file check (FAT has
no permissions); and a key file's CRLF line ends, UTF-8 BOM or the Shell's
UTF-16 are accepted, where OpenSSH refuses them.

Logging in
----------

::

    ssh -t -p 22 anyone@<host>

Any user name is accepted: there is one console, and the key or password is
what admits you. Only interactive sessions are served; ``ssh host cmd`` is
refused, because there is no private shell to run it in. Before the session
starts, ``ssh`` prints the pre-authentication banner -- the ``Banner`` file,
if one is set, then a line stating the console's size; a terminal smaller
than the console is shown a notice until it is enlarged. ``exit`` ends the
Shell for every session, as it would at the local console.

**Errors and logs.** A program's error lines (its standard error) reach the
sessions, as they reach a screen listed in the firmware's ``ErrOut``.
``sshd``'s own log -- logins, refusals, the console's size -- does not: as
OpenSSH's sshd logs to syslog and never to the user's terminal, while it
serves, ``sshd`` writes its log to a serial port, in the console's format.

**The serial console is left alone.** While ``sshd`` serves, a console
redirected to a serial port -- a terminal on the line, a BMC's
serial-over-LAN -- behaves exactly as before it started: the Shell's output
reaches it, and so does what is typed there. So by default (``AxlLogPort
auto``) the log goes to the first serial port the console does *not* draw
on, and when the console draws on every port -- one UART, as on most
machines -- the log is not written while ``sshd`` serves. ``sshd -v`` says
which, before the server starts::

    [DEBUG] sshd:log_port_open:NNNN: the log goes to serial port 1 while serving

The console's own port is never written unasked, because the firmware's
serial terminal draws by cursor position from a cursor model of its own: a
line it did not write scrolls the terminal under it, and from then on the
Shell's typed line and its output are painted on the wrong rows, over the
log lines -- with ``-v``, one per packet, which buries them.
``--log-port N`` (or ``AxlLogPort N``) puts the log on port *N* (0 is the
first, in the firmware's order) even when the console draws on it, for an
operator who wants it there and accepts that; ``--log-port none`` turns it
off. A port the machine does not have stops the start, naming it.

Wherever the serial log goes, the server also keeps its log in memory, the
most recent 8 KB of whole lines, from the moment it serves until it
unloads. ``sshd log`` prints it to the Shell::

    [INFO] ssh: Accepted publickey for anyone from 192.0.2.20 port 50122

When ``sshd stop`` fails, it prints the server's newest error line from
that log under "sshd: could not stop the server" -- in the Shell, after
serving has ended, so it disturbs nothing.

If the firmware will not give the console back -- the take-over cannot be
undone -- the driver stays resident with no server, and ``sshd status``
says ``sshd: stuck - the console take-over could not be undone; 'sshd stop'
tries again``. That happens in three places, and the service state says
which: a failed stop, or a start whose setup failed after it had taken the
console, leaves the service *stopping* (``status`` exits 5, and a new
``sshd`` refuses with the same line); a Shell that exits while ``sshd`` runs
and whose console cannot be released leaves it *degraded*. Each ``sshd
stop`` retries the release; a reset clears it.

**Keys** reach the Shell as a USB keyboard's do: a Ctrl, Shift or Alt
combination arrives with its modifiers, so ``edit`` answers Ctrl-End or
Shift-End with "Unknown Command", as it does at a USB keyboard. The bytes a
terminal sends for Backspace, Tab, Enter and Esc stay those keys, though
they are also Ctrl-H, Ctrl-I, Ctrl-M and Ctrl-[. Esc followed by a key is
Alt+key only when both arrive together, as a terminal sends Alt+key; an Esc
on its own stays Esc. An Esc held over from an earlier packet stays a bare
Esc unless the next byte is ``[`` or ``O`` -- the start of a cursor or
function key's sequence split across packets, which is decoded whole. Ctrl-C breaks a running script even behind keys the
script has not read.

sshd.conf
---------

``sshd`` reads its settings from ``sshd.conf`` beside ``sshd.efi``, in
OpenSSH's ``sshd_config(5)`` syntax and keywords, so a file -- or the lines
of one -- written for OpenSSH's sshd reads the same here::

    # sshd.conf
    Port 2222
    PasswordAuthentication no
    MaxAuthTries 2
    LoginGraceTime 1m30s
    Banner "my banner.txt"
    PerSourcePenaltyExemptList 10.0.2.2/32

**The shipped file.** The UEFI-tools tarball puts an ``sshd.conf`` beside
``sshd.efi`` that lists every keyword this ``sshd`` supports, commented out
at its default with a line on what it does -- OpenSSH's own
``sshd_config`` style -- so as shipped it changes nothing. Remove the ``#``
from a line to change that setting. ``AxlPassword`` has no default: its
line holds a placeholder, which ``sshd`` refuses
(``sshd.conf:<line>: AxlPassword: replace the placeholder with a password``),
so uncommenting it unchanged cannot leave a known password in place.

**Syntax.** One ``Keyword arguments`` per line, or ``Keyword=arguments``;
keywords in any case; ``#`` starts a comment; double or single quotes group
an argument with spaces in it; LF or CRLF line ends; UTF-8 (with or without
a BOM) or UTF-16LE -- which is what the Shell's own ``echo ... > file``
writes. Times are seconds or OpenSSH's ``1h30m`` form; yes/no in any case.
When a keyword appears twice, the first value wins and the later one is
still checked, as in OpenSSH.

**Precedence:** sshd's defaults, then ``sshd.conf``, then the command line
-- a flag given wins over the file, flag by flag. A ``-k`` file or a ``-P``
password given on the command line is meant to be used, so it also turns
its method on for that run: ``-P`` means ``PasswordAuthentication yes`` and
``-k`` means ``PubkeyAuthentication yes``, whatever the file says
(``sshd -T`` shows the result). ``-x`` replaces the file's
``PerSourcePenaltyExemptList`` rather than adding to it, and each ``-x``
range is checked as the file's are.

**Errors stop sshd.** An unknown keyword, a value out of range, or a
keyword ``sshd`` does not support makes it refuse to start, naming the file
and line::

    sshd.conf:2: Bad configuration option: Bogus
    sshd.conf:1: MaxSessions: 11 is out of range (1-10)
    sshd.conf:3: X11Forwarding is not supported by this sshd

A missing ``sshd.conf`` is not an error -- ``sshd`` runs on its defaults,
and does the same, saying ``sshd: cannot locate sshd.efi's volume; not
reading sshd.conf``, when it cannot tell which volume it was loaded from --
but one that is there and cannot be read (a directory of that name, a bad
volume) is: ``sshd: cannot read FS0:\sshd.conf``. A file named with ``-f``
must exist. A ``Banner`` or ``AuthorizedKeysFile`` the file names must be
readable when ``sshd`` starts, and is reported at its line. Both are read
as ``sshd.conf`` is: UTF-8, with or without a BOM, or UTF-16LE as the
Shell writes it -- a banner made with ``echo ... > fs0:\banner.txt`` or
``edit`` is sent as its text. A ``Banner`` that is not text (a NUL, bytes
that are not UTF-8, UTF-16 that does not convert) is refused:
``sshd.conf:6: Banner: FS0:\banner.txt is not UTF-8 text``. Every check runs
before ``sshd`` looks for a running server, so a broken ``sshd.conf`` is
reported even while the old settings are still serving.

**The "not supported" rule.** Every other keyword OpenSSH 9.9p1 documents
is refused by name, as not supported by this sshd, rather than as unknown:
the setting is real, this sshd lacks it. A setting the file names must
either work or stop ``sshd``; none is accepted and silently ignored. The
exceptions are OpenSSH's own: the keywords OpenSSH itself reads and ignores
(``UseLogin``, ``UsePrivilegeSeparation``, ``Protocol``, ...) are ignored
here too, with OpenSSH's warning (``sshd.conf:4: Deprecated option
UseLogin``), and its aliases (``ChallengeResponseAuthentication``,
``DSAAuthentication``, ...) are read as the keyword they name. ``Match`` and
``Include`` are not supported.

Keywords
--------

Ranges are inclusive; *time* is seconds or ``1h30m``. sshd's own settings
carry an ``Axl`` prefix, so they cannot clash with a future OpenSSH keyword.

.. list-table::
   :header-rows: 1
   :widths: 22 20 14 14 30

   * - Keyword
     - Value
     - Range
     - Default
     - Difference from OpenSSH 9.9p1
   * - ``Port``
     - number
     - 1-65535
     - 22
     - Once only: sshd listens on one port
   * - ``LoginGraceTime``
     - time
     - 1-3600
     - 60
     - OpenSSH: 120, and 0 means no limit
   * - ``MaxAuthTries``
     - number
     - 1-10
     - 3
     - Counts wrong passwords; OpenSSH: 6
   * - ``MaxSessions``
     - number
     - 1-10
     - 10
     -
   * - ``ClientAliveInterval``
     - time
     - 0-2592000
     - 60
     - 0 is off; OpenSSH's default is 0
   * - ``ClientAliveCountMax``
     - number
     - 0-2147483647
     - 3
     -
   * - ``UnusedConnectionTimeout``
     - ``none`` or time
     - 0-2592000
     - 300
     - OpenSSH: ``none``
   * - ``PubkeyAuthentication``
     - yes/no
     -
     - yes
     -
   * - ``PasswordAuthentication``
     - yes/no
     -
     - yes
     - The password is ``AxlPassword`` (or ``-P``)
   * - ``Banner``
     - ``none`` or a path
     - text, up to 3968 bytes of UTF-8
     - none
     - Read once when sshd starts, not per connection: restart to change.
       Sent ahead of the console-size line
   * - ``PerSourcePenaltyExemptList``
     - ``none`` or CIDR list
     - 8 ranges
     - none
     - IPv4 only
   * - ``LogLevel``
     - ``QUIET``, ``FATAL``, ``ERROR``, ``INFO``, ``VERBOSE``, ``DEBUG``,
       ``DEBUG1``-``DEBUG3``, any case
     -
     - not set (``INFO``)
     - Mapped onto sshd's log levels (below); ``-v`` and ``-d`` win over it
   * - ``AuthorizedKeysFile``
     - ``none`` or a path
     - one file
     - ``authorized_keys`` beside ``sshd.efi``, if present
     - One file, not a per-user list; no ``%h``/``%u`` tokens
   * - ``MaxStartups``
     - number
     - 1-16
     - 10
     - A single number: no ``start:rate:full``
   * - ``RekeyLimit``
     - ``default`` or a size, then optionally ``none``
     - 16 bytes and up
     - 1G
     - No time limit; 0 and ``default`` mean 1G
   * - ``PerSourcePenalties``
     - ``yes``, ``no``, or ``name:time`` ...
     - each 0-86400
     - ``authfail:5 noauth:1 grace-exceeded:10 max:600 min:15``
     - Only those five settings; ``min`` must be below ``max``
   * - ``AxlNic``
     - ``auto`` or a number
     -
     - auto
     - sshd's own (``-n``)
   * - ``AxlNetworkTimeout``
     - time
     - 1-86400
     - 60
     - sshd's own (``--net-timeout``): the network bring-up's deadline
   * - ``AxlPassword``
     - a string
     -
     - none
     - sshd's own (``-P``): the password, for any user name
   * - ``AxlReadOnly``
     - yes/no
     -
     - no
     - sshd's own (``-R``); implies ``AxlSftpReadOnly``
   * - ``AxlSftpReadOnly``
     - yes/no
     -
     - no
     - sshd's own (``--sftp-read-only``): SFTP refuses every write; see
       *SFTP*
   * - ``AxlReportGeometry``
     - yes/no
     -
     - no
     - sshd's own (``--report-geometry``)
   * - ``AxlLogPort``
     - ``auto``, ``none`` or a number
     - 0-65535
     - auto
     - sshd's own (``--log-port``): the serial port the log goes to while
       serving; see *The serial console is left alone*
   * - ``AxlTcpLinger``
     - a number (microseconds)
     - 0-20000
     - 0 (off)
     - sshd's own (``--tcp-linger``): how long a connection stays polled
       after a reply; see `Request/response latency: AxlTcpLinger`_
   * - ``Subsystem``
     - ``sftp`` and a command
     -
     - ``sftp SftpServer.efi`` with no file; none in a file that omits it
     - ``sftp`` only, once, no arguments to the command. The command is
       ``SftpServer.efi`` (or ``internal-sftp``, the same) or the helper's
       path, relative to ``sshd.efi``; see *SFTP*

**LogLevel.** sshd logs through AxlLog, which has five levels where
OpenSSH has nine, so the names map onto them:

=========================  ========================================
LogLevel                   sshd logs
=========================  ========================================
QUIET, FATAL, ERROR        errors
INFO                       information
VERBOSE, DEBUG, DEBUG1     debug: each step of the start, and more
DEBUG2, DEBUG3             trace
=========================  ========================================

QUIET still logs errors, so a start that fails says why. With no
``LogLevel`` (and no ``-v``), sshd sets no level at all, so ``set
AXL_LOG_LEVEL debug`` in the Shell still takes effect; ``sshd -T`` then
prints ``loglevel INFO``, OpenSSH's default. ``-v`` and ``-d`` are once
only: unlike OpenSSH's ``-d``, a second one is not DEBUG2 -- use
``LogLevel DEBUG2``.

sshd -T
-------

``sshd -T`` prints the settings ``sshd`` would start with -- defaults, the
file and the flags together -- one ``keyword value`` line each, in
OpenSSH's ``sshd -T`` format (lower-case keyword, ``yes``/``no``, seconds,
``none``), and exits without starting anything::

    FS0:\> sshd -T -f good.conf -p 2223
    port 2223
    logingracetime 90
    ...
    axlpassword set

It differs from OpenSSH's in five places:

- ``maxstartups`` is a single number, not ``start:rate:full``.
- ``persourcepenalties`` lists only the five settings sshd has, in
  OpenSSH's order; OpenSSH also prints ``crash``, ``refuseconnection``,
  ``max-sources4/6`` and ``overflow``/``overflow6``.
- ``rekeylimit`` prints the byte limit in force (``1073741824 0`` by
  default), where OpenSSH prints ``0 0`` for "the cipher's default".
- ``banner`` and ``authorizedkeysfile`` print the path made absolute,
  beside ``sshd.efi``, and ``axlpassword`` prints only ``set`` or
  ``none``.
- An integer too large for OpenSSH's ``int`` is refused as ``<Keyword>
  integer value too large.`` -- for ``Port`` too, where OpenSSH says
  ``Badly formatted port number.``

``-T`` reads and checks everything ``sshd`` does, so it is also the way to
test an edited ``sshd.conf`` without stopping a running server.
