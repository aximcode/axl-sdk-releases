AxlSsh — SSH server
===================

An SSH server over :doc:`AxlTcp <net>` and :doc:`AxlLoop <loop>`.

**P3 — publickey and password authentication.** A stock ``ssh -i key``
completes version exchange, KEXINIT, curve25519 key exchange and NEWKEYS
against this server, then authenticates by publickey against the keys given
to :c:func:`axl_ssh_server_add_authorized_key` or
:c:func:`axl_ssh_server_load_authorized_keys`, or by password, or both.
Password authentication is not built in: the application installs it through
:c:func:`axl_ssh_server_set_auth_ops`, and AxlSsh stores no credential of its
own. :c:func:`axl_ssh_server_listen` refuses to bind unless it can
authenticate somebody — an authorized key **or** a password hook.

**The login grace is settable**, through :c:func:`axl_ssh_server_set_login_grace`,
called before :c:func:`axl_ssh_server_listen` like
:c:func:`axl_ssh_server_set_auth_ops`. It replaces
``AXL_SSH_LOGIN_GRACE_DEFAULT_SEC`` (60 s) as the deadline from accept to
authenticated; zero is refused, since a disabled grace hands a peer back the
denial of service the grace exists to prevent, and so is anything past one
hour. A short grace also sizes the reaper's own sweep finer, so it is not
silently rounded up to a fixed 5 s lag — but that finer sweep then runs for
the server's whole life, not only near a deadline. Because authentication
hooks run one at a time server-wide, a connection waiting its turn keeps its
own grace running regardless, so a grace shorter than (connections queued) x
(the slowest hook's latency) can reap a login that did nothing wrong.

**The password-failure cap is settable, and it is LIVE** — AxlSsh's
counterpart of sshd's ``MaxAuthTries``, through
:c:func:`axl_ssh_server_set_max_password_failures`. It replaces
``AXL_SSH_PASSWORD_FAILURES_DEFAULT`` (3) and is bounded by
``AXL_SSH_PASSWORD_FAILURES_MAX`` (10); zero is refused, because "no
password authentication" means installing no password hook. Unlike
:c:func:`axl_ssh_server_set_login_grace` it may be called at any time from
the server's loop, including while the server is listening and from an
authentication hook: the cap is copied into each connection at accept and
the reply path reads that copy, so a change applies to connections accepted
afterwards and a connection already open keeps the cap it arrived under.
That is sshd's reload behaviour, which forks a process per connection with
the configuration then in force. It counts only guesses the application's
hook judged — never a publickey request, the ``none`` probe, or a password
AxlSsh itself refused before any hook saw it — where ``MaxAuthTries`` counts
every failed method.

**A pre-authentication banner is settable, and LIVE too** — sshd's
``Banner``, through :c:func:`axl_ssh_server_set_banner`, up to
``AXL_SSH_BANNER_MAX`` (4096 bytes, sized so the DoD Notice and Consent
Banner fits where ``AXL_SSH_AUTH_BANNER_MAX``'s 1024 would refuse it). It
is sent as ``SSH_MSG_USERAUTH_BANNER`` ahead of the reply to a connection's
FIRST authentication request, whatever user name or method that request
uses, and exactly once per connection; a connection whose first request
finds no banner set never gets one later. The text must be valid UTF-8,
which RFC 4252 section 5.4 specifies for the field — conformance caught at
configuration time, not a filter, since the RFC puts control-character
filtering on the client. A banner that cannot be sent DROPS the connection
rather than authenticating it silently. NULL or ``""`` removes it, which is
the default.

The verdict for either method — password, or the publickey principal — may
complete later rather than inline, through :c:func:`axl_ssh_auth_complete`.
That matters because a real password check is not free: SoftBMC's PBKDF2
costs 15,000 iterations, and running that on the server's own event loop
would stall every other connection for as long as the hash takes. A hook may
hand the request to :doc:`AxlAsync <task>` and answer from its done callback
on another core instead. RFC 4252 section 5.1 is enforced either way: a
connection reads nothing further once its request is sent, until a verdict
arrives.

Authentication hooks run **one at a time for the whole server**, queued in
arrival order: a connection that would read authentication input while a
hook is on the stack waits its turn instead. A hook that answers only from
:c:func:`axl_ssh_auth_complete` (never by blocking inline) costs the logins
behind it nothing worse than its own latency; a hook that DOES block holds
every other connection's authentication input unread for as long as it
runs, which is a real, server-wide cost with enough concurrent peer
connections. That was settled on 2026-09-26: hooks stay strictly serial,
and the stall is made VISIBLE instead of bounded.
:c:func:`axl_ssh_server_set_auth_hook_budget_ms` sets how long a hook may
hold the server before it is reported — one second by default, ``0`` to
report nothing, and **logging only**: nothing is refused or cancelled at
the budget, because a knob that changed enforcement would invite an
operator to set it and believe they were protected when the library cannot
take a hook off the stack. There are two lines, and they are not the same
event: one when a hook RETURNS over budget, carrying the method, the user,
the source, the hold and how many connections were queued behind it, and
one best-effort line while a hook is still running, at most one per call.
**A hook that blocks without dispatching the loop produces no mid-hook
line at all** — the sweep that would write it is dispatched from the stack
that hook is holding — so a hook that can genuinely wedge must be watched
from outside the loop (:c:func:`axl_watchdog_set`, or a platform
watchdog). A connection the login grace reaps while it was still waiting
its turn says so in its timeout line, rather than reading like a peer that
sent nothing. :c:func:`axl_ssh_server_free` may be
called from a hook, ``release_principal``, or anything their dispatch
runs; the server stops at once, except that the connection whose own hook
is currently running is closed — and its principal released, if it held
one — only once that hook returns, which can be after the free call has
already returned to its own caller. Once a server's free has started,
every other public call on it (:c:func:`axl_ssh_server_listen`,
:c:func:`axl_ssh_server_set_auth_ops`,
:c:func:`axl_ssh_server_set_login_grace`,
:c:func:`axl_ssh_server_set_penalties`,
:c:func:`axl_ssh_server_set_penalty_exempt`,
:c:func:`axl_ssh_server_set_max_password_failures`,
:c:func:`axl_ssh_server_set_banner`,
:c:func:`axl_ssh_server_set_auth_hook_budget_ms`, the authorized-key
loaders) is refused
rather than reaching a stale pointer; :c:func:`axl_ssh_auth_complete`
is refused too, but for its own reason — its request handles are unique
across the whole image, so a stale one matches nothing on any server. At
most 8 servers may be live in one process image.

A key not in the authorized set is refused **without dropping the
connection** — ``ssh`` walking its key list, not an attack — while a bad
signature under an authorized key ends the connection immediately, whether
or not the client asked about the key first. A wrong password gets the same
non-fatal refusal, but **the wrong password that reaches the connection's
cap ends it** — the third, with the defaults — and so does a request that
changes the user name (RFC 4252 section
5; sshd does the same); a publickey login whose
signature verifies but whose principal the application's hook refuses is an
ordinary failure too, not forgery. A password flagged for a required change
gets ``AXL_SSH_AUTH_CHANGE_FIRST`` — refused like a wrong password, counting
toward the same cap, but with a ``USERAUTH_BANNER`` sent first so the client
can say why; AxlSsh implements no change-password form itself.

**A source that keeps failing is refused for a while** — OpenSSH's
``PerSourcePenalties``, on by default at OpenSSH's own numbers, and measured
against a real ``sshd`` 9.9p1. Each connection that ends without
authenticating charges its source address: 5 s for a refused attempt by
either method (a forged signature included), 10 s for one the login grace
reaped, 1 s for one that attempted nothing; a successful login charges
nothing and clears nothing. Nothing is enforced until a source's penalty
exceeds 15 s — so an operator who mistypes, is disconnected and reconnects
is never refused — and from then on it is enforced until it has burned down
to nothing, capped at 10 minutes. A refused connection is reset before the
identification line, costs no connection slot and accrues nothing.
:c:func:`axl_ssh_server_set_penalties` tunes or disables it;
:c:func:`axl_ssh_server_set_penalty_exempt` names up to 8 CIDR ranges it
never applies to, such as the jump host or management LAN every operator
shares. A connection the server could not set up, or could not time, is
charged nothing; one it drops mid-parse for a failure of its own — a
signature it ran out of memory checking, say — is charged like a protocol
error, by what the peer did before it. Refusal
is an RST because a graceful close from the accept callback would wait
inline for TIME_WAIT on every refused connection. Its honest limit: a
patient guesser is throttled, not locked out; it is keyed by address, so
hosts behind one NAT share a penalty; the table tracks 256 sources, and a
guesser that keeps more than that many of its addresses tracked can leave
its next address untracked entirely; and it does not cover SoftBMC's web
login.

A failure is replied to no earlier than a per-user minimum borrowed from
OpenSSH (5 ms plus 0-4.29 ms derived from the user name), on one
server-wide reply timer. **Its honest limit**: this hides the reply's own
timing only when the application's check costs the same for every user — a
hook that returns early for an unknown user leaks exactly that difference.

**Session channels are opt-in.** Until the application installs channel
hooks with :c:func:`axl_ssh_server_set_channel_ops` (before listening), every
``CHANNEL_OPEN`` is refused with ``SSH_OPEN_UNKNOWN_CHANNEL_TYPE``. With them,
``session`` channels open — ``ssh host``, ``ssh -tt host``, ``ssh host cmd``
and sessions multiplexed through ``ssh -o ControlMaster`` — and the
application drives them through hooks and integer handles
(``AxlSshChannel``): a handle kept past its channel's close is
``AXL_NOT_FOUND``, never a use-after-free. Up to
``AXL_SSH_MAX_SESSIONS_DEFAULT`` (10, sshd's ``MaxSessions``) channels per
connection; the eleventh open draws ``SSH_OPEN_RESOURCE_SHORTAGE`` and the
connection stays up. :c:func:`axl_ssh_channel_write` is all-or-nothing and
bounded — ``AXL_BUSY`` past 256 KB per channel or 4 MB per server, with a
``writable`` hook once a retry would fit — and output leaves from the loop,
after the reply to the request whose hook wrote it. The ``close`` hook runs
exactly once per opened channel, before the connection's
``release_principal``. The header below is the contract.

**Inbound flow control can follow the application.** By default a channel's
window is reopened as bytes reach the ``data`` hook, so a peer is never
paced. :c:func:`axl_ssh_channel_set_window_mode` with
``AXL_SSH_CHANNEL_WINDOW_ON_CONSUME`` credits a delivered byte back only when
the application reports it with :c:func:`axl_ssh_channel_consumed` — so a
reader that falls behind stops its peer after one window (2 MB), as sshd
stops a client whose session process stops reading its pipe. The
application then holds what it has not consumed itself, at most
``AXL_SSH_CHANNEL_UNCONSUMED_MAX`` (the window plus sshd's 10% grace) per
channel; :c:func:`axl_ssh_channel_unconsumed` reads the count. Reports only
count: the ``WINDOW_ADJUST`` goes out from the loop, once the window is more
than three maximum packets below its size (OpenSSH's rule), however many
reports were made. sshd's ``sftp`` subsystem runs in this mode.

One algorithm per slot — ``curve25519-sha256``, ``ssh-ed25519``,
``chacha20-poly1305@openssh.com`` — which is a security decision rather than
an unfinished one: an algorithm matrix is a downgrade surface.

.. note::

   ``ssh-ed25519`` is the only host key algorithm. Its provider is opt-in for
   other images because its tables are ~120 KB, but
   :c:func:`axl_ssh_server_new` calls :c:func:`axl_pk_enable_ed25519` itself,
   so an AxlSsh application needs no flag and no call of its own.

API Reference
-------------

Core — role-neutral
~~~~~~~~~~~~~~~~~~~

Shared by the server and the future client. The packet codec, KEXINIT
builder and KDF are implemented but stay internal until a second role
consumes them.

.. doxygenfile:: axl-ssh-core.h

Server
~~~~~~

.. doxygenfile:: axl-ssh-server.h

Host key
~~~~~~~~

Where a server's identity is kept, how a provisioned key file is read, and
the fingerprint an operator compares with ``ssh-keygen -lf``. The functions
are split into a provider-free half, which a small launcher can link, and a
provider-bound half that needs the Ed25519 provider.

.. doxygenfile:: axl-ssh-host-key.h

Session channels
~~~~~~~~~~~~~~~~

.. doxygenfile:: axl-ssh-channel.h
