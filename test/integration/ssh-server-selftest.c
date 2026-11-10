/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* ssh-server-selftest.c -- the guest half of the AxlSsh P1 transport harness.
 *
 * Starts an AxlSshServer on a TCP port QEMU forwards to the host and pumps the
 * loop until a deadline. It asserts NOTHING: the assertions live on the host
 * side, in test-ssh-transport-qemu.sh, and they read a real ssh(1)'s -vvv
 * transcript. A guest that graded its own wire protocol would prove nothing
 * about what reaches a peer -- which is the entire reason this phase is tested
 * against OpenSSH rather than against a client of our own.
 *
 * A plain shell app (int main), public headers only.
 *
 * Output contract (exact lines the harness greps for):
 *   ssh: listening         -- accepting connections
 *   SERVER: FAIL <stage>   -- setup failed, and where (includes "authkeys")
 *   SERVER: DEADLINE       -- the deadline timer fired and quit the loop
 *   SERVER: FINISH-ARMED   -- (ctl=PORT) the host connected to the control
 *                             port, saying every scenario is over; the
 *                             finish timer (FINISH_MS) is armed
 *   SERVER: FINISH         -- that timer fired and quit the loop. Printed
 *                             INSTEAD of SERVER: DEADLINE when the host
 *                             signals: see "THE EARLY FINISH" below
 *   SERVER: CTL-FAIL <st>  -- the control listener's accept failed
 *   SERVER: REAPING        -- the loop returned (deadline or finish), freeing
 *   SERVER: DONE           -- the servers were freed cleanly
 *   SERVER: FAIL creds          -- the credentials file could not be loaded
 *   SERVER: PW-LEN <n>          -- a password hook was entered with a password
 *                                  of <n> bytes. The LENGTH only; no password
 *                                  byte is ever printed. "PW-LEN 0" can never
 *                                  appear, and that is the assertion
 *   SERVER: PW-ALLOC <count> <bytes> <user>
 *                               -- axl_mem_get_stats()'s CUMULATIVE totals on
 *                                  entry to a password hook. Printed for every
 *                                  one, so the difference between two
 *                                  consecutive lines for one user is
 *                                  everything the server allocated to carry
 *                                  one more packet on the SAME connection.
 *                                  That difference is what proves the inbound
 *                                  payload buffer is per connection and not
 *                                  per packet: a 35,000-byte buffer allocated
 *                                  per packet cannot hide inside it. The two
 *                                  NUMBERS COME FIRST because the host parses
 *                                  this record by column and the name is the
 *                                  peer's to choose. Graded on the host, in
 *                                  test-ssh-auth-qemu.sh
 *   SERVER: PRINCIPAL-RELEASED  -- release_principal ran (once per ACCEPT)
 *   SERVER: COMPLETE-STALE      -- a completion found nobody waiting
 *   SERVER: COMPLETE-ERR <rc>   -- a completion failed any other way
 *   SERVER: ITERATE-DONE <rc>   -- an "iterate" hook dispatched the loop for
 *                                  ITERATE_US, then completed inside itself;
 *                                  <rc> is axl_ssh_auth_complete's return
 *   SERVER: ITERATE-FAIL <irc> <rc>
 *                               -- the same hook, but the nested dispatch did
 *                                  not run its full duration (<irc> is
 *                                  axl_loop_iterate_until's return, and any
 *                                  value but AXL_TIMEOUT answers DENY) or no
 *                                  heartbeat fired inside it
 *   SERVER: ITERATE-PRINCIPAL-RELEASED
 *                               -- release_principal ran for the "iterate"
 *                                  principal, which is a different static
 *                                  from the one PRINCIPAL-RELEASED counts
 *   SERVER: NEST-ENTER <user>   -- a "nest" hook was entered, before it
 *                                  dispatches the loop for NEST_US
 *   SERVER: NEST-DONE <rc>      -- that hook dispatched for its full duration
 *                                  and completed inside itself (ACCEPT, with
 *                                  the PRINCIPAL-RELEASED principal)
 *   SERVER: NEST-FAIL <irc> <rc>
 *                               -- as ITERATE-FAIL, for a "nest" hook
 *   SERVER: BLOCK-DONE <rc>     -- a "block" hook BLOCKED for BLOCK_MS
 *                                  WITHOUT dispatching the loop, then
 *                                  completed. The one shape the hook
 *                                  watchdog cannot report mid-hook: the
 *                                  sweep that would write that line is
 *                                  dispatched from the stack this hook is
 *                                  holding. Its slow-RETURN line still
 *                                  lands, which is what makes the missing
 *                                  mid-hook line evidence rather than
 *                                  silence
 *   SERVER: BUDGET-SET <rc>     -- a "block" hook raised the hook budget to
 *                                  the maximum from INSIDE itself, before
 *                                  holding the server. It must not change
 *                                  the report about THAT hook, which
 *                                  latched the boot's value at entry
 *   SERVER: BUDGET-RESTORED <rc>
 *                               -- ...and the boot's value put back, from
 *                                  the server's loop, once that hook has
 *                                  returned
 *   SERVER: FREED-IN-HOOK       -- a "free" hook's timer freed the server
 *                                  from inside that hook's dispatch
 *   SERVER: FREE-DONE <rc>      -- the "free" hook dispatched for FREE_US and
 *                                  then completed against the server it was
 *                                  handed, which by then was STOPPED (removed
 *                                  from the live-server table, not yet
 *                                  actually freed -- this hook's own call is
 *                                  still on the stack); <rc> is
 *                                  axl_ssh_auth_complete's return, and it is
 *                                  refused the same way a completion against
 *                                  a genuinely freed server is -- that case
 *                                  is test_ssh_auth_complete_after_free
 *                                  (test/unit/axl-test-ssh.c)
 *   SERVER: FREE-FAIL <irc> <rc>
 *                               -- as ITERATE-FAIL, for a "free" hook
 *   SERVER: HOOK-DEPTH-MAX <n>  -- the most hooks (password or publickey)
 *                                  ever on the stack at once; printed at
 *                                  teardown, just before MEM-CORRUPTION
 *   SERVER: MEM-CORRUPTION <n>  -- axl_mem_corruption_count() after the
 *                                  quarantine is drained; printed just
 *                                  before SERVER: DONE
 *   SERVER: FAIL penalties      -- the penalty spec did not parse or was
 *                                  refused by axl_ssh_server_set_penalties
 *   SERVER: FAIL exempt         -- the exempt list was refused
 *   SERVER: FAIL second-<stage> -- the second (penalty) server failed setup
 *   SERVER: CAP-SET <rc> <n>    -- a "setcap" password hook called
 *                                  axl_ssh_server_set_max_password_failures
 *                                  from inside itself; printed ONCE per boot
 *   SERVER: BANNER-SET <rc>     -- a "banner" password hook called
 *                                  axl_ssh_server_set_banner from inside
 *                                  itself; printed ONCE per boot
 *   SERVER: BAD-BANNER <r1> <r2>
 *                               -- a "badban" password hook completed once
 *                                  with a banner that is not valid UTF-8
 *                                  (<r1> must be AXL_INVALID) and then
 *                                  completed the SAME request again with no
 *                                  banner (<r2> must be AXL_OK, which is
 *                                  what proves the first refusal left the
 *                                  request pending)
 *   SERVER: BANNER-OOM <rc>     -- an "oom" password hook called
 *                                  axl_ssh_server_set_banner with the next
 *                                  allocation rigged to fail. <rc> must be
 *                                  AXL_NO_RESOURCES, and the banner already
 *                                  in force must survive it. Printed ONCE
 *   SERVER: BANNER-AFTER-FREE <rc>
 *                               -- axl_ssh_server_set_banner called on the
 *                                  server a "free" hook has just freed. <rc>
 *                                  must be AXL_ERR: the server is off the
 *                                  live table, so the call must not read it
 *
 * Session channels (channels=1; see "THE FIXTURE SHELL" below):
 *   SERVER: CHANNEL-OPEN <h> <user> peer=<a.b.c.d>:<port> principal=<0|1>
 *       ud=<n>              -- an open hook accepted channel handle <h>.
 *                              Every field after <h> is read back through
 *                              an ACCESSOR (axl_ssh_channel_user, _peer,
 *                              _principal, _user_data), not from the
 *                              hook's arguments, so each accessor's answer
 *                              is asserted; <n> is the per-boot channel
 *                              counter attached with set_user_data
 *   SERVER: PRINCIPAL-MISMATCH  -- the hook's principal argument and
 *                              axl_ssh_channel_principal disagreed
 *   SERVER: CHANNEL-REFUSED <h> -- the open hook refused (user "refuse")
 *   SERVER: OPEN-WRITE <rc>     -- (user "openwrite") the open hook wrote
 *                              "opened\n"; <rc> is the write's return
 *   SERVER: OPEN-CLOSE <rc>     -- (user "openclose") the open hook closed
 *                              its channel, then accepted
 *   SERVER: OPEN-CLOSE-REFUSE <rc>
 *                           -- (user "opencloserefuse") the open hook
 *                              closed its channel, then REFUSED; it
 *                              attaches no state, so a close hook the
 *                              library promised not to run prints ud=0
 *   SERVER: OPEN-EXIT <rc>      -- (user "openexit") the open hook called
 *                              axl_ssh_channel_exit(0), then accepted
 *   SERVER: BLOB-BUSY <n>       -- an exec "blob" write of <n> bytes was
 *                              refused with AXL_BUSY
 *   SERVER: BLOB-DONE <n>       -- ...and was accepted, whole
 *   SERVER: STREAM-DONE <n>     -- an exec "stream" queued all <n> bytes
 *   SERVER: MEM <count> <bytes> -- the shell's "mem": the allocator's live
 *                              blocks and bytes (axl_mem_get_stats)
 *   SERVER: PTY <term> <cols>x<rows>
 *   SERVER: RESIZE <cols>x<rows>
 *   SERVER: EOF                 -- the eof hook ran
 *   SERVER: BUSY <n>            -- a "flood" write was refused with
 *                              AXL_BUSY after <n> bytes were accepted
 *   SERVER: WRITABLE            -- the writable hook ran
 *   SERVER: FLOOD-DONE <n>      -- a "flood" queued all <n> bytes
 *   SERVER: QUIT <rc>           -- "quit" closed its channel from inside
 *                              the data hook; <rc> is the close's return
 *   SERVER: FREE-RETURNED       -- "freeserver" freed the server from inside
 *                              the data hook, and the free returned
 *   SERVER: AFTER-FREE <rc>     -- a write on that channel right after the
 *                              free; must be AXL_NOT_FOUND
 *   SERVER: CHANNEL-CLOSE ud=<n> -- the close hook ran; <n> is read from
 *                              the hook's chan_data, so it proves the
 *                              pointer attached at open came back
 *   SERVER: CLOSE-LOOKUP <rc>   -- set_user_data on the closing handle from
 *                              INSIDE the close hook; must be NOT_FOUND
 *   SERVER: STALE-WRITE <h> <rc>
 *                           -- (stale=1) a write to closed channel <h>'s
 *                              handle from a timer 200 ms after its close
 *   SERVER: HOLD-EXIT <rc>      -- a "hold" timer's exit failed (the
 *                              channel was gone before it fired)
 *   SERVER: HOOK-NESTED         -- a hook of a channel was entered while
 *                              another hook of the SAME channel was on the
 *                              stack (the close hook included) -- which
 *                              the library promises never happens
 *   SERVER: CLOSE-OOM <rc>      -- "closeoom": axl_ssh_channel_close from a
 *                              timer with its CLOSE's allocation failing;
 *                              <rc> is the close's return
 *   SERVER: CLOSE-OOM-AFTER     -- printed by that timer right after the
 *                              close returned: the close hook must FOLLOW
 *                              it, never run inside the call
 *   SERVER: DATA-BEFORE-START <n>
 *                           -- the data hook ran, with <n> bytes, for a
 *                              channel whose shell or exec hook has not
 *                              accepted: the library promises it never
 *                              does (data before the start is discarded)
 *   SERVER: DATA-AFTER-EOF <n>  -- the data hook ran after the eof hook:
 *                              promised never to happen
 *   SERVER: DATA-NO-STATE <n>   -- the data hook ran for a handle with no
 *                              user data: one already retired
 *   SERVER: PAUSE-FAIL <irc>    -- a "pause" or "slowflood" dispatch of the
 *                              loop did not run its full duration
 *   SERVER: HELD                -- a "holdflood" writable hook or a
 *                              "holdpause" data hook, dispatching the loop,
 *                              saw the parse gate hold its connection's
 *                              input (see "THE FIXTURE SHELL")
 *   SERVER: HOLD-WAIT-FAIL <irc>
 *                           -- ...and it did not within HOLD_WAIT_US, or a
 *                              dispatch failed (<irc>; 0 = it timed out)
 *   SERVER: TX-REFUSED          -- a "txfail" hook's armed seam refused the
 *                              run a lingering connection handed TCP
 *   SERVER: TX-COMPLETE-FAILED  -- a "txcfail" hook's armed seam turned the
 *                              completion of a lingering connection's run
 *                              into a failure
 *   SERVER: WRITE-FAIL <rc>     -- a fixture write failed unexpectedly
 *   SERVER: EXIT-FAIL <rc>      -- a fixture exit failed unexpectedly
 *
 * Usage:
 *   ssh-server-selftest.efi [port] [deadline_ms] [authorized_keys|-] \
 *       [credentials] [login_grace_sec] [penalties|-] [exempt|-] [port2] \
 *       [grace2_sec] [name=value ...]
 *
 * The positional list ENDS at the first name=value OPTION (see
 * count_positional), so a caller supplies only the positional slots it
 * needs and then names the rest -- no "-" or 0 padding for slots it does
 * not care about, which is what nine positional arguments had started to
 * cost. Options may come in any order, and an UNKNOWN one is a hard
 * failure rather than an ignored typo. Every one but sshloglevel and
 * holdsend (which are the whole boot's) and the numbered per-server ones
 * (unusedN=, aliveN=, rekeylimitN=, channelsN=, portN=) acts on the FIRST
 * server only, and the option section starts at the first argument
 * containing an '=' (see count_positional), so a MISSPELLED one lands there
 * and is refused by name rather than silently ignored:
 *   maxpwfail=N       axl_ssh_server_set_max_password_failures(N) before
 *                     listen (D4)
 *   latency=M         axl_net_set_latency before listening, M one of
 *                     firmware | window | always (boot-wide; default: the
 *                     library's own default, window)
 *   maxstartups=N     axl_ssh_server_set_max_startups(N) before listen
 *                     (P4a admission). The second server keeps the shipped
 *                     AXL_SSH_MAX_STARTUPS_DEFAULT, so one boot can hold a
 *                     server at each cap
 *   capafter=N        the cap a "setcap" password hook installs from inside
 *                     itself, the first time one runs
 *   banner=PATH       axl_ssh_server_set_banner() with that FILE's exact
 *                     bytes, before listen (D9)
 *   bannerafter=PATH  the text a "banner" password hook installs from inside
 *                     itself, the first time one runs
 *   hookbudget=N      axl_ssh_server_set_auth_hook_budget_ms(N) before
 *                     listen (NEW-1). Unset leaves the shipped default;
 *                     N = 0 is a real value here (report nothing), which is
 *                     why "unset" is UINT32_MAX and not 0
 *   holdsend=N        axl_tcp_fail_next_send_schedule(N) before listen: the
 *                     Nth TCP send retirement in this boot keeps its
 *                     callback until its socket is closed, as when the
 *                     loop's defer queue and source table are both full.
 *                     Also raises the "tcp" log domain to DEBUG, where the
 *                     seam says it fired.
 *                     Process-wide, like sshloglevel -- it is how a test
 *                     holds a finished send open across a connection's
 *                     teardown without depending on timing
 *   channels=1        install the fixture shell's session-channel hooks
 *                     (axl_ssh_server_set_channel_ops) on the first server
 *                     before listen. Without it the server refuses every
 *                     CHANNEL_OPEN, as a server with no hooks does
 *   stale=1           every close hook also arms a 200 ms timer that writes
 *                     to the closed handle (SERVER: STALE-WRITE) -- the
 *                     application timer that outlives its channel
 *   writelimit=PER,TOTAL
 *                     axl_ssh_server_set_channel_write_limit(PER, TOTAL) on
 *                     the first server before listen. Unset leaves the
 *                     shipped 256 KB / 4 MB, which no scenario's output
 *                     reaches in total -- this is how a test makes the
 *                     server-wide total and the fair-share floor
 *                     (TOTAL / 160) bind
 *   rekeylimit=N      axl_ssh_server_set_rekey_limit(N) on the first server
 *                     before listen: bytes per key per direction (0 = never
 *                     by bytes). Unset leaves the shipped 1 GiB, which no
 *                     scenario reaches -- this is how a test makes the
 *                     SERVER start a rekey
 *   unused=SEC        axl_ssh_server_set_unused_timeout(SEC) on the first
 *                     server before listen (P4e); 0 is a real value (off).
 *                     Unset leaves the shipped 300 s
 *   alive=SEC:COUNT   axl_ssh_server_set_client_alive(SEC, COUNT) on the
 *                     first server before listen; SEC 0 is off, COUNT 0
 *                     probes and never drops. Unset leaves the shipped
 *                     60 s x 3
 *   unused2=SEC, alive2=SEC:COUNT, rekeylimit2=N
 *                     the same three for the SECOND server (port2), which
 *                     otherwise keeps the shipped values
 *   port3=P, port4=P, port5=P
 *                     start a THIRD, FOURTH and FIFTH server on these
 *                     ports, shaped exactly as the second (password logins
 *                     only, the same keys and penalties, nothing exempt,
 *                     the second's login grace), after the second and
 *                     before the heartbeat. One boot can then hold five
 *                     servers whose idle and rekey settings differ
 *   unused3=SEC, alive3=SEC:COUNT, rekeylimit3=N,
 *   unused4=SEC, alive4=SEC:COUNT, rekeylimit4=N,
 *   unused5=SEC, alive5=SEC:COUNT, rekeylimit5=N
 *                     the same three for the third, fourth and fifth.
 *   channels2=1, channels3=1, channels4=1, channels5=1
 *                     the fixture shell's channel hooks on that server too.
 *                     The commands that act later from a TIMER -- "hold",
 *                     "closeoom" and stale=1's write -- name the FIRST
 *                     server (g_chan_srv), so on another server only the
 *                     commands that act from inside a hook ("stream",
 *                     "echo", the line shell's) are meaningful
 *                     All the per-server options are one table
 *                     (g_srv_opt_names); a bad value fails as
 *                     SERVER: FAIL <unused|alive|rekeylimit|channels|port>-arg,
 *                     and a value its setter refuses as SERVER: FAIL
 *                     <unused|alive|rekeylimit> (second-, third- or
 *                     fourth- prefixed for the other servers)
 *   ctl=PORT          listen on PORT for the host's "every scenario is
 *                     over" signal (THE EARLY FINISH, below)
 *   sshloglevel=L     axl_log_set_domain_level("ssh", L) for the whole
 *                     boot; "debug" or "info". WHY A TEST NEEDS THIS: the
 *                     server's audit log is rate-limited to one INFO line
 *                     per KIND per minute and writes the rest at DEBUG, and
 *                     at the library's default level a DEBUG line is not
 *                     DEMOTED in the guest log, it is ABSENT. So every
 *                     invariant about how MANY lines of a kind a path
 *                     writes -- "at most one mid-hook line per hook call"
 *                     is the one that matters -- is unobservable without
 *                     it, and a regression that wrote three would look
 *                     identical to one that wrote one
 *
 * THE EARLY FINISH. The loop ends on its deadline timer, and the host grades
 * SERVER: DONE because a loop that stopped dispatching never fires a timer.
 * But the deadline is sized for the slowest arch's slowest passing run of
 * every scenario, so a healthy run used to idle out minutes of it. With
 * ctl=PORT the fixture also listens on a plain AxlTcp port of its own: the
 * host connects to it once every scenario and every check that reads the
 * live server is done, and the accept arms a FINISH_MS one-shot timer on the
 * same loop, through the same axl_loop_add_timeout the deadline uses. That
 * timer, not the accept, quits the loop -- so a loop that has lost its
 * timers still never prints SERVER: DONE. The deadline stays armed as the
 * CEILING: a host that never connects (an old caller, a crashed one, the
 * SSH_HOSTILE_ABUSE=0 control) gets exactly the deadline path. The listener
 * is its own socket, not an SSH server, because scenarios free their
 * servers (a "free" hook, "freeserver") and fill them to their caps.
 *
 * The banner comes from a FILE, not from an argv string, because the
 * rendering measurement stages LF, CRLF, a lone CR, TAB and ESC -- none of
 * which survives a shell argument, a UEFI Shell command line and this
 * parser intact.
 *
 * "-" for authorized_keys means "load none". A fourth argument installs the
 * P3 auth hooks (see below); without it the fixture behaves exactly as it
 * did for P1/P2. A fifth (Task G) calls axl_ssh_server_set_login_grace()
 * with that value before axl_ssh_server_listen(); without it the server
 * keeps AXL_SSH_LOGIN_GRACE_DEFAULT_SEC. Requires the fourth argument too --
 * there is no way to install a shorter grace without also installing hooks,
 * which no scenario needs one without the other for.
 *
 * Task P (per-source penalties) adds three, each optional and each "-" for
 * "leave it alone". Without them the server runs the SHIPPED penalties --
 * on, OpenSSH's defaults, exempting nothing -- which is what
 * test-ssh-auth-qemu.sh and test-ssh-transport-qemu.sh exercise.
 *   penalties  AUTHFAIL:NOAUTH:GRACE:MIN:MAX, seconds, handed to
 *              axl_ssh_server_set_penalties() for BOTH servers.
 *   exempt     comma-separated ranges for axl_ssh_server_set_penalty_exempt()
 *              on the FIRST server only.
 *   port2      starts a SECOND server on this port: the same penalties and
 *              authorized keys, NO exemption, password logins through
 *              on_password_b, so a scenario can be refused by it while the
 *              first server -- which every other scenario uses -- never shares
 *              its penalty table. Its keys are there so a FORGED signature
 *              can reach it (the penalty scenario's first failure).
 *   grace2_sec the second server's own login grace; without it, the first
 *              server's (login_grace_sec). Short, so the penalty scenario can
 *              afford to let one connection's grace run out.
 */

#include <axl.h>

#define PORT_DEFAULT         2222
#define DEADLINE_MS_DEFAULT  60000
#define CREDS_MAX            16
#define PENDING_MAX          32
#define TICK_MS              25
#define TICKS_NORMAL         2      /* ~50 ms: proves the verdict can arrive
                                        later */
#define TICKS_SLOW           120    /* ~3 s: long enough for a client to
                                        leave */
#define CHANGE_BANNER        "password change required: use the web UI"
/* How long an "iterate" hook dispatches the loop from inside itself. Long
   enough to span a reaper tick past the login grace in
   test-ssh-hostile-hooks-qemu.sh's auth-hook-reenter, whose comment carries
   the margin arithmetic -- change the two together. */
#define ITERATE_US           20000000u
/* How long a "nest" hook dispatches. Long enough that the host can release
   two more clients' passwords inside it (test-ssh-hostile-hooks-qemu.sh's
   auth-hook-nest polls the guest log once a second), and short enough that
   three of them in series (18 s) stay comfortably inside whatever login
   grace is in force: that file's GRACE_SEC (40 s, Task G), which it shares
   with auth-hook-reenter in one boot, or AXL_SSH_LOGIN_GRACE_DEFAULT_SEC
   (60 s) when nothing sets one. auth-hook-nest itself does not need a
   shorter grace -- it never waits one out -- so this constant was
   deliberately left alone rather than shrunk to match. */
#define NEST_US              6000000u
/* A "free" hook dispatches for FREE_US and frees the server FREE_AFTER_MS
   into that dispatch, so the free lands with the hook on the stack. */
#define FREE_US              3000000u
#define FREE_AFTER_MS        1000u
/* How long a "block" hook holds the server WITHOUT dispatching the loop
   (axl_msleep waits on one timer event; it runs no AxlLoop source). Longer
   than the reaper's 5 s tick ON PURPOSE: the point is that a sweep would
   have happened in this window had the loop been dispatched, so "no
   mid-hook line" is evidence about the watchdog and not about the clock.
   test-ssh-hostile-hooks-qemu.sh's auth-hook-reenter sets a 5 s budget with
   hookbudget=, which this must also exceed for the RETURN line -- the
   control that proves the hook really held the server -- to be written. */
#define BLOCK_MS             6000u
/* How long after the host's finish signal the loop quits (THE EARLY FINISH).
   A real timer rather than a quit from the accept, so the ending still
   proves the loop runs its timers; short, because it is paid every run. */
#define FINISH_MS            2000u

typedef struct {
    char user[64];
    char password[128];
    char mode[8];              /* "ok", "change", "slow", "iterate", "nest",
                                  "free", "block", "setcap", "banner", "oom",
                                  "txfail", "txcfail" or "badban" --
                                  seven characters is the ceiling this field
                                  sets, and axl_strlcpy truncates silently
                                  past it */
} Cred;

typedef struct {
    AxlSshAuthRequest req;     /* 0 = free slot */
    AxlSshAuthVerdict verdict;
    bool     with_principal;
    unsigned ticks_left;
} Pending;

static AxlSshServer *g_srv;   /* NULLed by on_free_in_hook once freed --
                                  do not read it past that without checking */
/* The servers after the first (see SERVERS): password-only, each NULL
   unless started; nothing but main's teardown ever frees them. [0] is the
   second server (port2). */
#define EXTRA_SERVERS 4u
#define SERVERS       (1u + EXTRA_SERVERS)
static AxlSshServer *g_srv_extra[EXTRA_SERVERS];
static AxlLoop      *g_loop;
static uint32_t      g_ctl_port;        /* ctl=PORT, 0 = no control port */
static AxlTcp       *g_ctl;             /* its listener */
static Cred          g_creds[CREDS_MAX];
static unsigned      g_ncreds;
static Pending       g_pending[PENDING_MAX];
static int           g_principal_tag;   /* ok/slow/publickey principal */
static int           g_iterate_tag;     /* the "iterate" principal: DISTINCT,
                                           so its release is counted apart */

/* The heartbeat's count. Declared up here because an "iterate" hook reads it
   to prove its nested dispatch really ran other sources. */
static unsigned      tick_count;

/* Hooks on the stack right now, and the most there ever were. The server
   runs one hook at a time; a hook that dispatches the loop must not have
   another nest inside it (P3 security review MED-3). */
static unsigned      g_hook_depth;
static unsigned      g_hook_depth_max;

/* Set once a "free" hook's timer has freed the server, so main does not free
   it a second time. */
static bool          g_srv_freed;

/* The name=value options (see the file header). g_banner and g_banner_after
   own heap copies of their files' bytes and are freed at teardown -- a leak
   here fails the run through common-test.sh's leak gate. */
static uint32_t      g_max_pw_fail;      /* maxpwfail=N, 0 = leave the default */
static uint32_t      g_max_startups;     /* maxstartups=N, 0 = leave the default */
static uint32_t      g_cap_after;        /* capafter=N */
/* hookbudget=N. UINT32_MAX means UNSET, not 0: 0 is a MEANINGFUL value for
   this one (report nothing), so it cannot double as "not given" the way
   g_max_pw_fail's 0 does. */
static uint32_t      g_hook_budget_ms = UINT32_MAX;
static uint32_t      g_hold_send;        /* holdsend=N, 0 = none */
/* sshloglevel=debug|info, or -1 for "leave the library's default". */
static int           g_ssh_log_level = -1;
static char         *g_banner;           /* banner=PATH, loaded */
static char         *g_banner_after;     /* bannerafter=PATH, loaded */
static bool          g_cap_set_done;     /* a "setcap" hook has run */
static bool          g_tx_refuse_armed;  /* a "txfail" hook armed the seam */
static bool          g_txc_fail_armed;   /* a "txcfail" hook armed the seam */
static bool          g_banner_set_done;  /* a "banner" hook has run */
static bool          g_banner_oom_done;  /* an "oom" hook has run */

/* Strong over the library's weak _axl_ssh_server_test_point, each point
   armed by a hook and fired ONCE -- at a cap of 1, on the run carrying the
   cap's DISCONNECT:
     "txfail"   the next run a LINGERING connection hands TCP is refused
     "txcfail"  the next run of a LINGERING connection that completes is
                reported to the server as failed */
bool
_axl_ssh_server_test_point(const char *point);

bool
_axl_ssh_server_test_point(const char *point)
{
    if (g_tx_refuse_armed && axl_strcmp(point, "tx-refuse-linger") == 0) {
        g_tx_refuse_armed = false;
        axl_printf("SERVER: TX-REFUSED\n");
        return true;
    }
    if (g_txc_fail_armed && axl_strcmp(point, "tx-complete-fail-linger") == 0) {
        g_txc_fail_armed = false;
        axl_printf("SERVER: TX-COMPLETE-FAILED\n");
        return true;
    }
    return false;
}

/* A file's exact bytes as a NUL-terminated string, or NULL. The banner
   options take a PATH for the reason the file header gives: their content is
   control characters that no argument vector survives. A NUL anywhere in the
   file would truncate the result here -- which is correct, since
   axl_ssh_server_set_banner() takes a C string and would refuse one anyway
   (axl_utf8_validate).

   NULL means "unreadable OR out of memory", and the caller's
   SERVER: FAIL banner-file covers both. Separating them would need a second
   line for a case no scenario distinguishes. */
static char *
load_text(const char *path)
{
    void  *buf = NULL;
    size_t len = 0;

    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        return NULL;
    }
    char *out = axl_malloc(len + 1);
    if (out != NULL) {
        axl_memcpy(out, buf, len);
        out[len] = '\0';
    }
    axl_free(buf);
    return out;
}

/* @a arg as "<name>=<value>", or NULL when it is not this option. The
   `arg[n] == '='` test is what keeps "banner" from swallowing
   "bannerafter". */
static const char *
opt_value(const char *arg, const char *name)
{
    size_t n = axl_strlen(name);

    return (axl_strncmp(arg, name, n) == 0 && arg[n] == '=') ? arg + n + 1
                                                             : NULL;
}

/* How many of @a argv are POSITIONAL: the index of the first argument that
   CONTAINS an '=', or @a argc.

   The alternative was to fix the options at slot ten and make every caller
   pad the gap -- and this fixture's own history says what that costs: the
   positional list reached nine by accretion, and a scenario wanting only a
   banner would have had to spell out four placeholders it does not care
   about, each one a chance to put a value in the wrong slot in silence. No
   positional argument here can collide, since none ever contains an '=':
   they are ports, millisecond counts, "-", dotted quads, and fsN: paths.

   '=' RATHER THAN THE KNOWN-NAME LIST, and the difference is a silent
   misconfiguration. Stopping at the first KNOWN name means a MISSPELLED
   option is not an option at all, so with all nine positional slots already
   filled it falls past the end of the loop below and is IGNORED -- the
   server boots configured differently from what the test believes, which is
   the exact failure the hard-failure branch exists to prevent.
   test-ssh-hostile-qemu.sh already fills all nine, so that was one scenario
   away from biting. Stopping at any '=' puts every misspelling inside the
   option section, where the else branch refuses it by name. */
static int
count_positional(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (axl_strchr(argv[i], '=') != NULL) {
            return i;
        }
    }
    return argc;
}

/* user:password:mode per line. The test generates every password per run, so
   none is committed. Fields contain no ':'. */
static bool
load_creds(const char *path)
{
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        return false;
    }
    char  *p     = (char *)buf;
    size_t start = 0;
    /* NOT `&& g_ncreds < CREDS_MAX` in the loop condition, which is what
       this was: it simply STOPPED, so a ninth credential vanished in
       silence and the scenario that needed it failed for a reason nobody
       could see from the log. A full list now fails loudly, as below. */
    for (size_t i = 0; i <= len; i++) {
        if (i != len && p[i] != '\n') {
            continue;
        }
        char   line[256];
        size_t n = i - start;
        if (n > 0 && p[start + n - 1] == '\r') {
            n--;
        }
        if (n > 0 && n < sizeof line) {
            axl_memcpy(line, p + start, n);
            line[n] = '\0';
            char *c1 = axl_strchr(line, ':');
            char *c2 = c1 != NULL ? axl_strchr(c1 + 1, ':') : NULL;
            if (c2 != NULL) {
                if (g_ncreds >= CREDS_MAX) {
                    axl_printf("SERVER: FAIL creds-overflow\n");
                    axl_free(buf);
                    return false;
                }
                *c1 = '\0';
                *c2 = '\0';
                Cred *cr = &g_creds[g_ncreds++];
                axl_strlcpy(cr->user, line, sizeof cr->user);
                axl_strlcpy(cr->password, c1 + 1, sizeof cr->password);
                axl_strlcpy(cr->mode, c2 + 1, sizeof cr->mode);
            }
        }
        start = i + 1;
    }
    axl_free(buf);
    return g_ncreds > 0;
}

static void
defer(AxlSshAuthRequest req, AxlSshAuthVerdict v, bool with_principal,
      unsigned ticks)
{
    for (unsigned i = 0; i < PENDING_MAX; i++) {
        if (g_pending[i].req == 0) {
            g_pending[i] = (Pending){ req, v, with_principal, ticks };
            return;
        }
    }
    axl_printf("SERVER: FAIL pending-full\n");
}

/* A hook that runs the server's loop from INSIDE itself -- the nested wait
   include/axl/axl-loop.h documents as safe from any source callback, and
   exactly what a hook checking a password against a remote service does.
   Every other source keeps firing meanwhile, including the server's reaper.
   Then it answers inside the hook, as a synchronous check would.

   The wait is a fixed duration with no done event, so AXL_TIMEOUT is the
   ONLY result that means it waited. Anything else -- AXL_NO_RESOURCES when
   the loop could not register the timeout, which waits on nothing at all --
   is answered DENY: a consumer copying this hook must not learn to accept a
   login over a wait that did not happen.

   `user` is NOT read after the dispatch: it lives in the connection, and the
   connection is what the dispatch can end. The server address, snapshotted
   before the dispatch below, is completed against even when a "free" hook's
   own axl_ssh_server_free() call has already run (and cleared the global
   g_srv) inside this same dispatch: the completion must then be refused
   without the server being read (P3 security review MED-4). What this call
   proves is the STOPPED case -- the server is already off the live-server
   table, but its memory is still intact because this hook's own call is
   still on the stack. It does NOT exercise a dangling pointer (P3 security
   re-review NEW-6): that case, where the memory itself has been released, is
   test_ssh_auth_complete_after_free in test/unit/axl-test-ssh.c, which runs
   against the DEBUG quarantine's poison fill and would fault on a version
   that walked the freed server's connection list.

   `what` names the output lines: <what>-DONE <rc> or <what>-FAIL <irc> <rc>. */
static void
dispatch_then_complete(AxlSshAuthRequest req, uint64_t wait_us,
                       void *principal, const char *what)
{
    /* Snapshot the server BEFORE dispatching: the "free" mode's timer frees
       it (and clears the global g_srv, see on_free_in_hook) from INSIDE the
       wait below. The completion after that must still be aimed at the
       ADDRESS axl_ssh_server_free() just acted on -- that is what proves the
       STOPPED case documented above check_password's "free" branch, not a
       NULL-argument check, and test-ssh-hostile-hooks-qemu.sh pins the
       exact result (SERVER: FREE-DONE -5) that only the real address
       produces. */
    AxlSshServer *srv          = g_srv;
    unsigned      ticks_before = tick_count;
    AxlStatus     irc = axl_loop_iterate_until(g_loop, NULL, wait_us);
    bool          waited     = irc == AXL_TIMEOUT;
    bool          dispatched = waited && tick_count > ticks_before;
    int           rc = waited
        ? axl_ssh_auth_complete(srv, req, AXL_SSH_AUTH_ACCEPT, principal,
                                NULL)
        : axl_ssh_auth_complete(srv, req, AXL_SSH_AUTH_DENY, NULL, NULL);
    if (dispatched) {
        axl_printf("SERVER: %s-DONE %d\n", what, rc);
    } else {
        axl_printf("SERVER: %s-FAIL %d %d\n", what, (int)irc, rc);
    }
}

/* Free the server from inside a hook's dispatch -- the call P3 security
   review MED-1 found spinning for ever, and what a web handler that turns SSH
   off does when it happens to run while a password check waits. */
static bool
on_free_in_hook(void *data)
{
    (void)data;
    axl_ssh_server_free(g_srv);
    /* WHILE THE GLOBAL STILL HOLDS THE ADDRESS, deliberately: that address
       is what proves axl_ssh_server_set_banner refuses a server whose free
       has started WITHOUT reading one byte of it. AXL_ERR, from the
       live-server table alone -- the same check axl_ssh_auth_complete makes,
       and the same reason (this hook's own call is still on the stack, so
       the memory is intact but the server is gone). A version that stored
       through the pointer would leak the copy it made, which the teardown
       leak report would then show. */
    axl_printf("SERVER: BANNER-AFTER-FREE %d\n",
               axl_ssh_server_set_banner(g_srv, "after the free"));
    /* axl_ssh_server_free()'s own header says not to keep a pointer to a
       server past freeing it -- a NEW server could later live at this same
       address. Clearing the global here means on_complete_tick (below) never
       touches it again; dispatch_then_complete's own pending completion for
       THIS hook already took its snapshot before this call ran, so clearing
       the global does not disturb it. */
    g_srv       = NULL;
    g_srv_freed = true;
    axl_printf("SERVER: FREED-IN-HOOK\n");
    return AXL_SOURCE_REMOVE;
}

/* Put the boot's hook budget back after a "block" hook raised it from
   inside itself (M6). Armed before that hook's sleep and dispatched from
   the SERVER's loop, so it cannot run until the hook has returned and
   app_call_end has already decided whether to report the hold -- which is
   exactly what makes the raise a test of the LATCH rather than of the
   setter. Without the restore the raised budget would silence the 20 s
   hook's own reports later in the same boot. */
static bool
on_restore_budget(void *data)
{
    (void)data;
    axl_printf("SERVER: BUDGET-RESTORED %d\n",
               axl_ssh_server_set_auth_hook_budget_ms(g_srv,
                                                      g_hook_budget_ms));
    return AXL_SOURCE_REMOVE;
}

/* Bracket every hook body, so the depth counts hooks and nothing else. */
static void
hook_enter(void)
{
    if (++g_hook_depth > g_hook_depth_max) {
        g_hook_depth_max = g_hook_depth;
    }
}

static void
hook_leave(void)
{
    g_hook_depth--;
}

static void
check_password(AxlSshAuthRequest req, const char *user, const char *password,
               size_t password_len)
{
    /* Compared HERE, while `password` is valid, and answered later -- so the
       test proves an asynchronous verdict without this fixture ever keeping
       a copy of a password. "iterate", "nest" and "free" answer inside this
       call instead, but only after dispatching the loop, and they compare
       before that too. */
    for (unsigned i = 0; i < g_ncreds; i++) {
        Cred *cr = &g_creds[i];
        if (axl_strcmp(cr->user, user) != 0) {
            continue;
        }
        if (axl_strlen(cr->password) != password_len
            || axl_memcmp(cr->password, password, password_len) != 0) {
            break;
        }
        if (axl_strcmp(cr->mode, "iterate") == 0) {
            dispatch_then_complete(req, ITERATE_US, &g_iterate_tag, "ITERATE");
        } else if (axl_strcmp(cr->mode, "nest") == 0) {
            axl_printf("SERVER: NEST-ENTER %s\n", user);
            dispatch_then_complete(req, NEST_US, &g_principal_tag, "NEST");
        } else if (axl_strcmp(cr->mode, "free") == 0) {
            /* No principal: this completion is expected to be REFUSED
               (the server it names has been freed), so there would be
               nobody to hand one back. */
            if (axl_loop_add_timeout(g_loop, FREE_AFTER_MS, on_free_in_hook,
                                     NULL) == 0) {
                axl_printf("SERVER: FAIL free-timer\n");
            }
            dispatch_then_complete(req, FREE_US, NULL, "FREE");
        } else if (axl_strcmp(cr->mode, "block") == 0) {
            /* HOLDS the server without dispatching it -- the one stall the
               watchdog's mid-hook report cannot see, since the reaper sweep
               that would write that line is dispatched from this very
               stack. The RETURN report still lands, and that is the whole
               design of the assertion pair in
               test-ssh-hostile-hooks-qemu.sh: without it, "no mid-hook
               line" would be satisfied by a hook that was simply fast.

               axl_msleep, not axl_loop_iterate_until. Be precise about what
               it does: axl_msleep -> axl_wait_ms CREATES AND RUNS A SECOND
               LOOP of its own (src/event/axl-wait.c) -- what it does not do
               is dispatch the SERVER's loop, which is the only thing this
               mode needs. It therefore trips AXL's warn-first re-entrancy
               guard deliberately, and every run of this mode carries
               `[WARN] wait: synchronous wait invoked from inside a loop
               callback` in the guest log. THAT GUARD IS PLANNED TO BECOME A
               REFUSAL (AXL_BUSY), which will break this mode -- loudly, and
               in the safe direction: the hook would return at once, its
               slow-return line would not be written, and the host's
               `block_slow` assertion would fall to 0 rather than the
               negative silently passing for the wrong reason. Replace the
               sleep with a bare spin then, not with a loop dispatch. */
            /* M6: the budget's LATCH, from inside the hook that is being
               measured -- the analogue of what "setcap" does for the
               password cap. Raised to the maximum HERE, so a version that
               read s->auth_hook_budget_ms at report time instead of the
               copy app_call_begin latched would find an hour's budget and
               write no slow-return line at all. The restore is armed BEFORE
               the sleep and runs from the SERVER's loop, which cannot be
               dispatched until this hook has returned and app_call_end has
               already made its decision -- so the ordering is structural,
               not a race. */
            axl_printf("SERVER: BUDGET-SET %d\n",
                       axl_ssh_server_set_auth_hook_budget_ms(
                           g_srv, AXL_SSH_AUTH_HOOK_BUDGET_MAX_MS));
            if (axl_loop_add_timeout(g_loop, 1, on_restore_budget, NULL)
                == 0) {
                axl_printf("SERVER: FAIL budget-restore-timer\n");
            }
            axl_msleep(BLOCK_MS);
            axl_printf("SERVER: BLOCK-DONE %d\n",
                       axl_ssh_auth_complete(g_srv, req, AXL_SSH_AUTH_ACCEPT,
                                             &g_principal_tag, NULL));
        } else if (axl_strcmp(cr->mode, "setcap") == 0) {
            /* D4 LIVE, from inside a hook: raise the cap once, then refuse
               this password like any other wrong one. The connection whose
               hook this is keeps the cap it was ACCEPTED with, so the count
               of prompts it gets is the per-connection copy's evidence; the
               next connection gets capafter=. The password MATCHED to get
               here -- the verdict is DENY regardless, which is what lets one
               connection spend its whole cap without the fixture holding a
               list of wrong passwords. */
            if (!g_cap_set_done) {
                g_cap_set_done = true;
                int rc = axl_ssh_server_set_max_password_failures(g_srv,
                                                                  g_cap_after);
                axl_printf("SERVER: CAP-SET %d %u\n", rc,
                           (unsigned)g_cap_after);
            }
            defer(req, AXL_SSH_AUTH_DENY, false, TICKS_NORMAL);
        } else if (axl_strcmp(cr->mode, "badban") == 0) {
            /* axl_ssh_auth_complete refuses a banner that is not valid
               UTF-8 -- and a PEER can trigger that, because the user name a
               hook is handed is never checked for UTF-8 and an application
               may build a banner from it. What the contract owes such an
               application is that the request is STILL PENDING, so it can
               complete again without the banner. Proving that needs a live
               request, which is why it is here and not in AxlTestSsh: the
               second completion must return AXL_OK and the login must
               actually succeed. */
            int bad = axl_ssh_auth_complete(g_srv, req, AXL_SSH_AUTH_DENY,
                                            NULL, "\xC0\x80 overlong");
            int ok  = axl_ssh_auth_complete(g_srv, req, AXL_SSH_AUTH_ACCEPT,
                                            &g_principal_tag, NULL);
            axl_printf("SERVER: BAD-BANNER %d %d\n", bad, ok);
        } else if (axl_strcmp(cr->mode, "oom") == 0) {
            /* D9's all-or-nothing return: a set_banner that cannot copy
               must leave the PREVIOUS banner in force. The copy is the only
               allocation the call makes, so the injected failure is it.
               Disarmed again right after, so a later allocation cannot
               inherit it -- an injection that outlives its target fails
               something unrelated and reads as a defect there. */
            if (!g_banner_oom_done) {
                g_banner_oom_done = true;
                axl_mem_fail_next_alloc(1);
                int rc = axl_ssh_server_set_banner(g_srv, g_banner_after);
                axl_mem_fail_next_alloc(0);
                axl_printf("SERVER: BANNER-OOM %d\n", rc);
            }
            defer(req, AXL_SSH_AUTH_ACCEPT, true, TICKS_NORMAL);
        } else if (axl_strcmp(cr->mode, "banner") == 0) {
            /* D9 LIVE, from inside a hook. ACCEPT, so this connection makes
               exactly one authentication request: its own banner decision
               was taken before this hook ran, so it must NOT see the new
               text, and the next connection must. */
            if (!g_banner_set_done) {
                g_banner_set_done = true;
                int rc = axl_ssh_server_set_banner(g_srv, g_banner_after);
                axl_printf("SERVER: BANNER-SET %d\n", rc);
            }
            defer(req, AXL_SSH_AUTH_ACCEPT, true, TICKS_NORMAL);
        } else if (axl_strcmp(cr->mode, "change") == 0) {
            defer(req, AXL_SSH_AUTH_CHANGE_FIRST, false, TICKS_NORMAL);
        } else if (axl_strcmp(cr->mode, "txfail") == 0) {
            /* P5: refused, and the NEXT run a lingering connection hands TCP
               is refused too -- at a cap of 1, the run carrying the cap's
               DISCONNECT (_axl_ssh_server_test_point below). */
            g_tx_refuse_armed = true;
            defer(req, AXL_SSH_AUTH_DENY, false, TICKS_NORMAL);
        } else if (axl_strcmp(cr->mode, "txcfail") == 0) {
            /* P5 (final review F2): accepted, and the completion of the next
               run a LINGERING connection hands TCP is reported as a failure
               (_axl_ssh_server_test_point above). Accepted rather than
               refused at the cap: ssh(1) hangs up the moment it reads a
               DISCONNECT, and that FIN can beat the completion -- then the
               seam would stay armed for whichever connection lingers next.
               A raw peer that never hangs up consumes it instead. */
            g_txc_fail_armed = true;
            defer(req, AXL_SSH_AUTH_ACCEPT, true, TICKS_NORMAL);
        } else if (axl_strcmp(cr->mode, "slow") == 0) {
            defer(req, AXL_SSH_AUTH_ACCEPT, true, TICKS_SLOW);
        } else {
            defer(req, AXL_SSH_AUTH_ACCEPT, true, TICKS_NORMAL);
        }
        return;
    }
    defer(req, AXL_SSH_AUTH_DENY, false, TICKS_NORMAL);
}

static void
on_password(AxlSshAuthRequest req, const char *user, const char *password,
            size_t password_len, void *ud)
{
    (void)ud;
    /* The LENGTH only, never a byte of the password itself. It is what
       test-ssh-auth-qemu.sh reads to prove an empty password never reaches a
       hook (axl_ssh_password_reaches_hook): the test asserts PW-LEN 0 is
       absent while a real length is present, so the negative is anchored to
       a line the same run proves this fixture really prints. */
    axl_printf("SERVER: PW-LEN %u\n", (unsigned)password_len);
    /* The allocator's cumulative totals. What the host grades is the
       DIFFERENCE between consecutive lines for one user, which is one
       connection's cost of carrying one more packet -- see the output contract
       at the top. Read here rather than at teardown because a total for the
       whole boot cannot say what scaled with what.

       The span between two lines contains exactly one hook body either way
       (reading at entry includes this hook's, reading at exit would include
       the next one's), and it costs nothing: defer() only fills a slot in a
       static table. THE USER NAME COMES LAST, after both numbers, because the
       host parses this record BY COLUMN and a peer chooses the name -- a name
       with a space in it would otherwise shift the numeric fields.
       on_userauth refuses only a NUL and an over-length name, not
       whitespace. */
    AxlMemStats mem;
    axl_mem_get_stats(&mem);
    axl_printf("SERVER: PW-ALLOC %zu %zu %s\n", mem.total_count,
               mem.total_bytes, user);
    hook_enter();
    check_password(req, user, password, password_len);
    hook_leave();
}

static void
on_publickey(AxlSshAuthRequest req, const char *user, const uint8_t *blob,
             size_t blob_len, void *ud)
{
    (void)blob;
    (void)blob_len;
    (void)ud;
    hook_enter();
    /* Answered INSIDE the hook, on purpose: the other completion path. */
    bool refuse = axl_strcmp(user, "nokey") == 0;
    int  rc     = axl_ssh_auth_complete(
                      g_srv, req,
                      refuse ? AXL_SSH_AUTH_DENY : AXL_SSH_AUTH_ACCEPT,
                      refuse ? NULL : &g_principal_tag, NULL);
    if (rc != AXL_OK) {
        axl_printf("SERVER: COMPLETE-ERR %d\n", rc);
    }
    hook_leave();
}

/* The second server's password hook. Answered INSIDE the hook against the
   same credentials file, whatever the credential's mode: ACCEPT carries the
   counted principal, so a login on this server is accounted for at teardown
   exactly like every other, and everything else is DENY. @a ud is the server
   (set_auth_ops' user_data), so this never touches g_srv. Not bracketed by
   hook_enter: it never dispatches the loop, and the depth metric is the
   FIRST server's one-hook-at-a-time rule, which is per server. */
static void
on_password_b(AxlSshAuthRequest req, const char *user, const char *password,
              size_t password_len, void *ud)
{
    bool ok = false;

    for (unsigned i = 0; i < g_ncreds; i++) {
        const Cred *cr = &g_creds[i];
        if (axl_strcmp(cr->user, user) == 0) {
            ok = axl_strlen(cr->password) == password_len
                 && axl_memcmp(cr->password, password, password_len) == 0;
            break;
        }
    }
    int rc = axl_ssh_auth_complete((AxlSshServer *)ud, req,
                                   ok ? AXL_SSH_AUTH_ACCEPT : AXL_SSH_AUTH_DENY,
                                   ok ? &g_principal_tag : NULL, NULL);
    if (rc != AXL_OK) {
        axl_printf("SERVER: COMPLETE-ERR %d\n", rc);
    }
}

/* "AUTHFAIL:NOAUTH:GRACE:MIN:MAX" into @a out. False on anything else. */
static bool
parse_penalties(const char *spec, AxlSshPenalties *out)
{
    char   **f  = axl_strsplit(spec, ':');
    uint32_t v[5];
    bool     ok = f != NULL;

    for (unsigned i = 0; ok && i < 5; i++) {
        ok = f[i] != NULL && axl_str_to_u32(f[i], 10, &v[i], NULL) == AXL_OK;
    }
    ok = ok && f[5] == NULL;
    axl_strfreev(f);
    if (ok) {
        *out = (AxlSshPenalties){ v[0], v[1], v[2], v[3], v[4] };
    }
    return ok;
}

/* The comma-separated exempt list, through the public setter. */
static bool
apply_exempt(AxlSshServer *srv, const char *list)
{
    char **f = axl_strsplit(list, ',');
    size_t n = 0;

    if (f == NULL) {
        return false;
    }
    while (f[n] != NULL) {
        n++;
    }
    int rc = axl_ssh_server_set_penalty_exempt(srv, (const char *const *)f, n);
    axl_strfreev(f);
    return rc == AXL_OK;
}

static void
on_release(void *principal, void *ud)
{
    (void)ud;
    if (principal == &g_principal_tag) {
        axl_printf("SERVER: PRINCIPAL-RELEASED\n");
    } else if (principal == &g_iterate_tag) {
        axl_printf("SERVER: ITERATE-PRINCIPAL-RELEASED\n");
    }
}

/* The per-server options (see the file header), one entry per server:
   [0] the first, [1] the second (port2), [2], [3] and [4] the third,
   fourth and fifth (port3=, port4=, port5=). The have_ flags say an option was given: 0 is a real
   value for every field here (off, never drop, never rekey by bytes). */
typedef struct {
    uint32_t unused_sec;
    uint32_t alive_sec;
    uint32_t alive_max;
    uint32_t rekey_limit;
    uint32_t port;          /* [2] to [4] only: 0 = not started */
    bool     channels;      /* channels=1 / channelsN=1 */
    bool     have_unused;
    bool     have_alive;
    bool     have_rekey_limit;
} SrvOpts;

static SrvOpts       g_srv_opts[SERVERS];

/* The kinds of per-server option, in the order of g_srv_opt_names' columns.
   OPT_PORT has no name for the first two servers: theirs are positional. */
enum { OPT_UNUSED, OPT_ALIVE, OPT_REKEYLIMIT, OPT_CHANNELS, OPT_PORT,
       OPT_KINDS };

/* Each server's spelling of each kind: the first server's bare, the others
   suffixed with the server's number. NULL = not an option of that server. */
static const char *const g_srv_opt_names[SERVERS][OPT_KINDS] = {
    { "unused",  "alive",  "rekeylimit",  "channels",  NULL    },
    { "unused2", "alive2", "rekeylimit2", "channels2", NULL    },
    { "unused3", "alive3", "rekeylimit3", "channels3", "port3" },
    { "unused4", "alive4", "rekeylimit4", "channels4", "port4" },
    { "unused5", "alive5", "rekeylimit5", "channels5", "port5" },
};

/* What each kind's failure is called (SERVER: FAIL <kind>-arg), whichever
   server it was given for. */
static const char *const g_srv_opt_kind[OPT_KINDS] = {
    "unused", "alive", "rekeylimit", "channels", "port",
};

/* "SEC:COUNT" into @a o's alive pair. Whether the values are ones the setter
   accepts is the setter's to say, at setup. */
static bool
parse_alive(const char *spec, SrvOpts *o)
{
    char   **f  = axl_strsplit(spec, ':');
    uint32_t v[2];
    bool     ok = f != NULL;

    for (unsigned i = 0; ok && i < 2; i++) {
        ok = f[i] != NULL
             && axl_str_to_u32(f[i], 10, &v[i], NULL) == AXL_OK;
    }
    ok = ok && f[2] == NULL;
    axl_strfreev(f);
    if (ok) {
        o->alive_sec  = v[0];
        o->alive_max  = v[1];
        o->have_alive = true;
    }
    return ok;
}

/* @a arg as one server's option of g_srv_opt_names. Returns NULL when it
   is not one, "" when it was parsed into g_srv_opts, and otherwise the
   kind that failed to parse. */
static const char *
parse_srv_opt(const char *arg)
{
    for (unsigned s = 0; s < SERVERS; s++) {
        for (unsigned k = 0; k < OPT_KINDS; k++) {
            const char *name = g_srv_opt_names[s][k];
            const char *v    = name != NULL ? opt_value(arg, name) : NULL;
            SrvOpts    *o    = &g_srv_opts[s];
            bool        ok   = true;

            if (v == NULL) {
                continue;
            }
            switch (k) {
            case OPT_UNUSED:
                ok = axl_str_to_u32(v, 10, &o->unused_sec, NULL) == AXL_OK;
                o->have_unused = ok;
                break;
            case OPT_ALIVE:
                ok = parse_alive(v, o);
                break;
            case OPT_REKEYLIMIT:
                ok = axl_str_to_u32(v, 10, &o->rekey_limit, NULL) == AXL_OK;
                o->have_rekey_limit = ok;
                break;
            case OPT_CHANNELS:
                ok = axl_strcmp(v, "1") == 0;
                o->channels = ok;
                break;
            default:
                ok = axl_str_to_u32(v, 10, &o->port, NULL) == AXL_OK
                     && o->port != 0 && o->port <= 65535u;
                break;
            }
            return ok ? "" : g_srv_opt_kind[k];
        }
    }
    return NULL;
}

/* Apply whichever of @a o's settings were given to @a srv, before listen.
   On a refusal, *@a stage is the setter's own stage -- "unused", "alive" or
   "rekeylimit" -- so SERVER: FAIL names which one refused, and false is
   returned. The setters are independent of each other (each only records a
   setting the server reads at accept), so their order carries nothing; it
   follows the option table's columns, and the first that refuses is the
   one named. */
static bool
apply_srv_opts(AxlSshServer *srv, const SrvOpts *o, const char **stage)
{
    if (o->have_unused
        && axl_ssh_server_set_unused_timeout(srv, o->unused_sec) != AXL_OK) {
        *stage = "unused";
    } else if (o->have_alive
               && axl_ssh_server_set_client_alive(srv, o->alive_sec,
                                                  o->alive_max) != AXL_OK) {
        *stage = "alive";
    } else if (o->have_rekey_limit
               && axl_ssh_server_set_rekey_limit(srv, o->rekey_limit)
                      != AXL_OK) {
        *stage = "rekeylimit";
    } else {
        return true;
    }
    return false;
}

/* One of the servers after the first (@a i: 0 the second, 1 the third, 2 the
   fourth): password logins only, the given penalties, nothing exempt, and
   that server's g_srv_opts, with @a chan_ops as its channel hooks when
   channelsN=1 (NULL: every CHANNEL_OPEN refused). Returns the failing
   stage -- the step, prefixed with which server, e.g. "second-listen" --
   or NULL. */
static const char *
start_extra_server(unsigned i, AxlLoop *loop, uint32_t port,
                   uint32_t grace_sec, const AxlSshPenalties *pen,
                   const char *keys, const AxlSshChannelOps *chan_ops)
{
    static const char *const which[EXTRA_SERVERS] = {
        "second", "third", "fourth", "fifth",
    };
    static char   stage[32];
    const char   *step = NULL;
    AxlSshServer *srv  = axl_ssh_server_new(loop, (uint16_t)port);

    g_srv_extra[i] = srv;
    if (srv == NULL) {
        step = "new";
    } else if (keys != NULL
               && axl_ssh_server_load_authorized_keys(srv, keys) != AXL_OK) {
        step = "authkeys";
    } else if (axl_ssh_server_set_auth_ops(
                   srv, &(AxlSshAuthOps){ on_password_b, NULL, on_release },
                   srv) != AXL_OK) {
        step = "auth-ops";
    } else if (grace_sec != 0
               && axl_ssh_server_set_login_grace(srv, grace_sec) != AXL_OK) {
        step = "grace";
    } else if (pen != NULL
               && axl_ssh_server_set_penalties(srv, pen) != AXL_OK) {
        step = "penalties";
    } else if (g_srv_opts[1u + i].channels && chan_ops != NULL
               && axl_ssh_server_set_channel_ops(srv, chan_ops, srv)
                      != AXL_OK) {
        step = "channel-ops";
    } else if (!apply_srv_opts(srv, &g_srv_opts[1u + i], &step)) {
        /* step names the setter: "third-rekeylimit", say. */
    } else if (axl_ssh_server_listen(srv) != AXL_OK) {
        step = "listen";
    }
    if (step == NULL) {
        return NULL;
    }
    axl_snprintf(stage, sizeof stage, "%s-%s", which[i], step);
    return stage;
}

static bool
on_complete_tick(void *data)
{
    (void)data;
    /* g_srv is NULL once auth-hook-free has freed the server (see
       on_free_in_hook): every g_pending[] slot by then is either already
       empty or belongs to a connection the free already dropped, so this
       models axl_ssh_server_free()'s "do not keep a pointer past freeing it"
       rather than reaching for the address again. */
    if (g_srv == NULL) {
        return AXL_SOURCE_CONTINUE;
    }
    for (unsigned i = 0; i < PENDING_MAX; i++) {
        Pending *pd = &g_pending[i];
        if (pd->req == 0 || --pd->ticks_left > 0) {
            continue;
        }
        AxlSshAuthRequest req = pd->req;
        pd->req = 0;   /* free the slot BEFORE completing: completion can
                          resume parsing and call on_password again */
        int rc = axl_ssh_auth_complete(
                     g_srv, req, pd->verdict,
                     pd->with_principal ? &g_principal_tag : NULL,
                     pd->verdict == AXL_SSH_AUTH_CHANGE_FIRST
                         ? CHANGE_BANNER : NULL);
        if (rc == AXL_NOT_FOUND) {
            /* The client left first. The principal was not adopted, so this
               fixture still owns it -- a static, nothing to free. */
            axl_printf("SERVER: COMPLETE-STALE\n");
        } else if (rc != AXL_OK) {
            /* Unreachable today: the only allocating path here is the
               CHANGE_FIRST banner, and CHANGE_BANNER always fits. But
               axl_ssh_auth_complete()'s header is explicit that
               AXL_NO_RESOURCES means the request is still PENDING, unlike
               every other non-AXL_OK case here -- `req` was never consumed.
               This fixture already cleared `pd->req` above (on purpose, to
               avoid re-entering itself from the resumed parse) so it cannot
               retry from this slot; a REAL consumer must not discard its
               own bookkeeping the way this fixture does until
               axl_ssh_auth_complete() returns something other than
               AXL_NO_RESOURCES, so it can retry the same req later instead
               of abandoning it the way COMPLETE-ERR does here. */
            axl_printf("SERVER: COMPLETE-ERR %d\n", rc);
        }
    }
    return AXL_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------------
   THE FIXTURE SHELL (channels=1): just enough application behind a session
   channel to drive every test-ssh-session-qemu.sh scenario, each output line
   exact. Every hook takes the SERVER as its ud (set_channel_ops' user data),
   never g_srv, so a hook that frees the server touches nothing global.

   open    accepts, except for user "refuse" (ADMINISTRATIVELY_PROHIBITED,
           and no close hook may follow). Attaches a ChanState. Three more
           users act from INSIDE the open hook after accepting --
           "openwrite" writes "opened\n", "openclose" closes the channel,
           "openexit" calls axl_ssh_channel_exit(0) -- and
           "opencloserefuse" closes it and then refuses, with no ChanState
           attached.
   exec    a table: "echo T" writes T\n INSIDE the hook and exits 0;
           "err T" writes T\n to stderr, exit 3; "true" 0; "false" 1;
           "hold MS" exits 0 from a loop timer MS later -- never a
           blocking stall, so the reaper keeps ticking while it runs;
           "cat" copies data back until EOF, then exits 0; "sink" hashes
           every byte of input and at EOF writes "\nsink <bytes> <sha256 hex>\n" and exits 0 --
           input the peer can count without any of it echoed back;
           "sinkflood" writes SINK_BYTES of '.' as
           "slowflood" does (its writable hook dispatches the loop) while
           hashing every byte of input, and once both the flood and the
           input are done writes "\nsink <bytes> <sha256 hex>\n" and exits
           0; "blob N" makes ONE write of N bytes of 'b' (N up to
           BLOB_MAX) and exits 0 once it is accepted, retrying the same
           whole write from the writable hook after an AXL_BUSY; "stream N"
           writes N bytes, byte k of the stream being k % 251 -- a pattern
           that is not periodic in any packet size, so a lost, repeated or
           reordered packet changes the host's SHA-256 -- 4 KB at a time,
           resuming on writable, and exits 0 once all of it is queued;
           "hoard N ACT" is "sink" on a channel in
           AXL_SSH_CHANNEL_WINDOW_ON_CONSUME (set in the exec hook, which
           prints SERVER: HOARD with what the window calls return there --
           an invalid mode, a NULL output, a report of 0 and of 1 with
           nothing held, and the idle unconsumed count); it consumes
           nothing until N bytes (N > 0) are held unconsumed, and then, from
           a loop timer HOARD_MS later, prints SERVER: HOARD-TIMER and acts:
           "all" reports everything held in one call, "split" in
           HOARD_SPLIT calls, "arrival" switches back to ON_ARRIVAL, "none"
           does nothing; the timer re-arms the next time N are held.
           "inline" reports each buffer from inside the data hook instead.
           At EOF it prints SERVER: HOARD-EOF with the unconsumed count and
           ends as "sink" does; "adjoom N" is "sink" whose data hook, once
           exactly N bytes have arrived, raises the "mem" log domain to
           DEBUG (the allocator names what it fails there) and rigs the
           SECOND allocation from then on to fail (SERVER: ADJOOM armed):
           with N three maximum packets and the next data packet already in
           the same receive, the first is that packet's decryption and the
           second the WINDOW_ADJUST it makes due, built inline as it is
           parsed;
           anything else is "unknown command\n" on stderr and exit 127.
   subsystem  prints SERVER: SUBSYSTEM <name> <len> and accepts only "cat",
           which copies data back as exec "cat" does; every other name is
           refused. Only the FIRST server has it: the extra servers
           (port2/port3=/port4=) install the same hooks with no subsystem
           hook, which is how a suite shows a server without one.
   shell   a line shell over the data hook: it echoes input (a line end as
           \r\n) and runs each complete line: "echo T" -> T\r\n; "exit N";
           "quit" closes the channel from INSIDE the data hook; "flood N"
           writes N bytes of 'x' in 4 KB writes until AXL_BUSY and resumes
           on writable; "slowflood N" is "flood" in '.' bytes whose
           writable hook first DISPATCHES THE LOOP for SLOW_US -- the
           application that runs the loop from a hook, which is what the
           parse gate is for; "pause MS" dispatches the loop for MS from
           INSIDE the data hook, then goes on reading the same buffer;
           "holdflood N" is "flood" whose FIRST writable hook dispatches the
           loop until the parse gate holds this connection's input, and
           "holdpause" dispatches the loop from the data hook until it does
           (both print SERVER: HELD): a hook that runs the loop exactly as
           long as it takes the peer's next packet to arrive under it, so a
           scenario orders a held packet against a hook by construction, not
           by timing. They count the server's DEBUG "input held" line through
           a log handler, so they need sshloglevel=debug;
           "stderr T" -> T\r\n on stderr; "mem" prints SERVER: MEM with
           the allocator's live blocks and bytes, so a test can compare
           two quiet points of one session; "freeserver" frees the server
           from inside the data hook; "closeoom" closes the channel from a
           timer with the allocation its CLOSE needs rigged to fail.
   Every hook counts itself in its channel's ChanState (`depth`) and prints
   SERVER: HOOK-NESTED if another hook of the same channel is already on
   the stack. The data hook prints DATA-BEFORE-START, DATA-AFTER-EOF or
   DATA-NO-STATE when the library delivers data it promised not to.
   ------------------------------------------------------------------------- */

/* The largest write "flood" makes. */
#define FLOOD_CHUNK  4096u
/* How long a "slowflood" writable hook dispatches the loop first. */
#define SLOW_US      200000u
/* The longest a "holdflood" or "holdpause" hook waits for the gate to hold
   input, and the slice it dispatches the loop in while it waits. */
#define HOLD_WAIT_US   15000000u
#define HOLD_SLICE_US  20000u
/* The server's DEBUG line for each time the parse gate starts holding a
   connection's input (axl-ssh-server.c, pump). */
#define HOLD_LINE    "chan: input held while a channel hook runs"
/* How much "sinkflood" writes. */
#define SINK_BYTES   2000000u
/* How long after "hoard" holds its N bytes its timer acts, and how many
   reports "split" makes. */
#define HOARD_MS     200u
#define HOARD_SPLIT  8u
/* The largest "blob" write: past any per-channel limit a writelimit= test
   sets, so a blob is never refused for its size alone. */
#define BLOB_MAX     (128u * 1024u)

typedef struct {
    size_t        flood_left; /* bytes a "flood" still has to queue */
    size_t        flood_done; /* ...and has queued so far */
    size_t        line_len;
    unsigned      n;          /* the per-boot counter: SERVER: ... ud=<n> */
    unsigned      depth;      /* this channel's hooks on the stack */
    AxlChecksum  *sink;       /* "sinkflood": the input's SHA-256, or NULL */
    size_t        sink_bytes; /* ...and how many bytes it has hashed */
    size_t        blob;       /* "blob": the write still to be accepted */
    size_t        stream_left; /* "stream": bytes still to queue */
    size_t        stream_done; /* ...and queued so far: the pattern offset */
    size_t        hoard_at;   /* "hoard": unconsumed bytes that arm the
                                 timer; 0 = never */
    char          hoard_act;  /* "hoard": 'a'll, 's'plit, 'r' (arrival),
                                 'n'one, 'i'nline; 0 = not hoarding */
    bool          hoard_armed; /* its timer is pending */
    size_t        adjoom_at;  /* "adjoom": sink_bytes that arm the failure;
                                 0 = never */
    AxlSshChannel ch;
    bool          shell;      /* the data hook is the line shell */
    bool          slow;       /* the flood is a "slowflood" */
    bool          hold;       /* "holdflood": its next writable hook waits
                                 for the gate to hold input */
    bool          sink_eof;   /* "sink(flood)": its input has ended */
    bool          started;    /* the shell or exec hook accepted */
    bool          eof;        /* the eof hook ran */
    bool          cat;        /* exec "cat": data is copied back */
    bool          last_cr;    /* the previous byte ended a line with \r */
    bool          ended;      /* exited or closed: ignore further input */
    char          line[128];
} ChanState;

/* Any server has channels=1 / channelsN=1: the HOLD_LINE handler is
   installed. */
static bool          g_channels;
static bool          g_stale;        /* stale=1 */
static unsigned      g_chan_count;
/* The loop is running: a close hook arms its stale timer only then, so a
   close at teardown (after axl_loop_run returned) leaves no source behind. */
static bool          g_loop_running;
/* How many times the parse gate has started holding input, this boot: the
   server's HOLD_LINE, counted by on_ssh_log. */
static volatile unsigned g_gate_holds;
/* The first server's ADDRESS, kept after a "freeserver" frees it: the
   stale and hold timers name it on purpose -- the contract says a call on
   a freed server is recognised without being read. */
static AxlSshServer *g_chan_srv;
static char          g_flood_bytes[FLOOD_CHUNK];
static char          g_slow_bytes[FLOOD_CHUNK];
static char          g_blob_bytes[BLOB_MAX];
/* "stream"'s pattern, FLOOD_CHUNK + 251 bytes of i % 251, so any chunk of the
   stream is one contiguous slice of it starting at (offset % 251). */
static char          g_stream_bytes[FLOOD_CHUNK + 251u];
/* writelimit=PER,TOTAL; 0 = leave the shipped limits. */
static size_t        g_write_per;
static size_t        g_write_total;

/* "PER,TOTAL", both non-zero, into g_write_per and g_write_total. Whether
   the pair is one the setter accepts is the setter's to say, at setup. */
static bool
parse_write_limit(const char *spec)
{
    char   **f  = axl_strsplit(spec, ',');
    uint32_t v[2];
    bool     ok = f != NULL;

    for (unsigned i = 0; ok && i < 2; i++) {
        ok = f[i] != NULL && axl_str_to_u32(f[i], 10, &v[i], NULL) == AXL_OK
             && v[i] > 0;
    }
    ok = ok && f[2] == NULL;
    axl_strfreev(f);
    if (ok) {
        g_write_per   = v[0];
        g_write_total = v[1];
    }
    return ok;
}

/* Bracket a hook body: count it on its channel, and report a nested one. */
static ChanState *
chan_enter(ChanState *st)
{
    if (st != NULL && st->depth++ > 0) {
        axl_printf("SERVER: HOOK-NESTED\n");
    }
    return st;
}

static void
chan_leave(ChanState *st)
{
    if (st != NULL) {
        st->depth--;
    }
}

/* Dispatch the server's loop for @a us from inside a hook. */
static void
chan_dispatch(uint64_t us)
{
    AxlStatus irc = axl_loop_iterate_until(g_loop, NULL, us);
    if (irc != AXL_TIMEOUT) {
        axl_printf("SERVER: PAUSE-FAIL %d\n", (int)irc);
    }
}

/* Count the parse gate's holds (HOLD_LINE). A log handler: it may not
   allocate or log, and does neither. The whole message, exactly: a handler
   receives it formatted with no prefix and no newline (AxlLogHandler). */
static void
on_ssh_log(int level, const char *domain, const char *message,
           const AxlRealtime *stamp, void *data)
{
    (void)level;
    (void)stamp;
    (void)data;
    if (domain != NULL && message != NULL && axl_strcmp(domain, "ssh") == 0
        && axl_strcmp(message, HOLD_LINE) == 0) {
        g_gate_holds++;
    }
}

/* Dispatch the server's loop from inside a hook until the parse gate holds
   input once more. g_gate_holds is server-wide: any connection whose hook
   is on the stack can raise it. The scenarios that use this run ONE
   connection with a hook at a time, so there the hold is this hook's own
   connection's input received while it runs -- a property of the
   scenario, not of this function. */
static void
chan_await_hold(void)
{
    unsigned start = g_gate_holds;

    for (uint64_t waited = 0; waited < HOLD_WAIT_US;
         waited += HOLD_SLICE_US) {
        AxlStatus irc = axl_loop_iterate_until(g_loop, NULL, HOLD_SLICE_US);
        if (irc != AXL_TIMEOUT) {
            axl_printf("SERVER: HOLD-WAIT-FAIL %d\n", (int)irc);
            return;
        }
        if (g_gate_holds != start) {
            axl_printf("SERVER: HELD\n");
            return;
        }
    }
    axl_printf("SERVER: HOLD-WAIT-FAIL 0\n");
}

static void
chan_write(AxlSshServer *srv, AxlSshChannel ch, bool err, const void *buf,
           size_t len)
{
    int rc = err ? axl_ssh_channel_write_stderr(srv, ch, buf, len)
                 : axl_ssh_channel_write(srv, ch, buf, len);
    if (rc != AXL_OK) {
        axl_printf("SERVER: WRITE-FAIL %d\n", rc);
    }
}

static void
chan_exit(AxlSshServer *srv, AxlSshChannel ch, uint32_t status)
{
    int rc = axl_ssh_channel_exit(srv, ch, status);
    if (rc != AXL_OK) {
        axl_printf("SERVER: EXIT-FAIL %d\n", rc);
    }
}

static void
chan_sink_finish(AxlSshServer *srv, ChanState *st);

/* Queue what is left of a "flood", 4 KB at a time, until it is all queued
   or a write is refused -- then wait for the writable hook. */
static void
chan_flood(AxlSshServer *srv, ChanState *st)
{
    while (st->flood_left > 0) {
        size_t n  = st->flood_left < FLOOD_CHUNK ? st->flood_left
                                                 : FLOOD_CHUNK;
        int    rc = axl_ssh_channel_write(srv, st->ch,
                                          st->slow ? g_slow_bytes
                                                   : g_flood_bytes, n);
        if (rc == AXL_BUSY) {
            axl_printf("SERVER: BUSY %zu\n", st->flood_done);
            return;
        }
        if (rc != AXL_OK) {
            axl_printf("SERVER: WRITE-FAIL %d\n", rc);
            st->flood_left = 0;
            return;
        }
        st->flood_done += n;
        st->flood_left -= n;
    }
    axl_printf("SERVER: FLOOD-DONE %zu\n", st->flood_done);
    chan_sink_finish(srv, st);
}

/* "sinkflood": once the flood is queued and the input has ended, report what
   arrived and exit. A refused report waits for the writable hook. */
static void
chan_sink_finish(AxlSshServer *srv, ChanState *st)
{
    if (st->sink == NULL || st->flood_left > 0 || !st->sink_eof) {
        return;
    }
    char line[128];
    int  n = axl_snprintf(line, sizeof line, "\nsink %zu %s\n",
                          st->sink_bytes, axl_checksum_get_string(st->sink));
    if (n <= 0 || axl_ssh_channel_write(srv, st->ch, line, (size_t)n)
                      != AXL_OK) {
        return;
    }
    axl_checksum_free(st->sink);
    st->sink = NULL;
    chan_exit(srv, st->ch, 0);
}

/* "blob": the one write, whole; exit 0 once it is accepted. A refusal is
   retried from the writable hook, the same length, so each BLOB-BUSY owes
   exactly one WRITABLE. */
static void
chan_blob(AxlSshServer *srv, ChanState *st)
{
    int rc = axl_ssh_channel_write(srv, st->ch, g_blob_bytes, st->blob);
    if (rc == AXL_BUSY) {
        axl_printf("SERVER: BLOB-BUSY %zu\n", st->blob);
        return;
    }
    size_t n = st->blob;
    st->blob = 0;
    if (rc != AXL_OK) {
        axl_printf("SERVER: WRITE-FAIL %d\n", rc);
        chan_exit(srv, st->ch, 1);
        return;
    }
    axl_printf("SERVER: BLOB-DONE %zu\n", n);
    chan_exit(srv, st->ch, 0);
}

/* "stream": queue what is left, 4 KB at a time, until it is all queued or a
   write is refused -- then wait for the writable hook. Exit 0 at the end. */
static void
chan_stream(AxlSshServer *srv, ChanState *st)
{
    while (st->stream_left > 0) {
        size_t n  = st->stream_left < FLOOD_CHUNK ? st->stream_left
                                                  : FLOOD_CHUNK;
        int    rc = axl_ssh_channel_write(srv, st->ch,
                                          g_stream_bytes
                                              + st->stream_done % 251u, n);
        if (rc == AXL_BUSY) {
            return;
        }
        if (rc != AXL_OK) {
            axl_printf("SERVER: WRITE-FAIL %d\n", rc);
            st->stream_left = 0;
            chan_exit(srv, st->ch, 1);
            return;
        }
        st->stream_done += n;
        st->stream_left -= n;
    }
    axl_printf("SERVER: STREAM-DONE %zu\n", st->stream_done);
    chan_exit(srv, st->ch, 0);
}

/* "closeoom": the close from a timer -- so no hook of the connection is on
   the stack -- with the allocation its CLOSE packet needs failing. */
static bool
on_close_oom(void *data)
{
    AxlSshChannel ch = (AxlSshChannel)(uintptr_t)data;

    axl_mem_fail_next_alloc(1);
    int rc = axl_ssh_channel_close(g_chan_srv, ch);
    axl_mem_fail_next_alloc(0);
    axl_printf("SERVER: CLOSE-OOM %d\n", rc);
    axl_printf("SERVER: CLOSE-OOM-AFTER\n");
    return AXL_SOURCE_REMOVE;
}

static bool
on_hold_done(void *data)
{
    int rc = axl_ssh_channel_exit(g_chan_srv, (AxlSshChannel)(uintptr_t)data,
                                  0);
    if (rc != AXL_OK) {
        axl_printf("SERVER: HOLD-EXIT %d\n", rc);
    }
    return AXL_SOURCE_REMOVE;
}

static bool
on_stale_write(void *data)
{
    AxlSshChannel ch = (AxlSshChannel)(uintptr_t)data;

    axl_printf("SERVER: STALE-WRITE %u %d\n", (unsigned)ch,
               axl_ssh_channel_write(g_chan_srv, ch, "late", 4));
    return AXL_SOURCE_REMOVE;
}

static int
chan_on_open(AxlSshChannel ch, void *principal, const char *user, void *ud)
{
    AxlSshServer *srv = ud;

    if (axl_strcmp(user, "refuse") == 0) {
        axl_printf("SERVER: CHANNEL-REFUSED %u\n", (unsigned)ch);
        return AXL_ERR;
    }
    /* Closed, then refused: the header folds the close into the refusal
       and promises no close hook. Nothing is attached, so a close hook
       that ran anyway would print ud=0 rather than read freed state. */
    if (axl_strcmp(user, "opencloserefuse") == 0) {
        axl_printf("SERVER: OPEN-CLOSE-REFUSE %d\n",
                   axl_ssh_channel_close(srv, ch));
        return AXL_ERR;
    }
    ChanState *st = axl_new(ChanState);
    if (st == NULL) {
        return AXL_NO_RESOURCES;
    }
    st->n  = ++g_chan_count;
    st->ch = ch;
    if (axl_ssh_channel_set_user_data(srv, ch, st) != AXL_OK) {
        axl_free(st);
        return AXL_ERR;
    }
    uint8_t         a[4] = { 0, 0, 0, 0 };
    uint16_t        port = 0;
    int             prc  = axl_ssh_channel_peer(srv, ch, a, &port);
    void           *pr   = axl_ssh_channel_principal(srv, ch);
    const char     *u    = axl_ssh_channel_user(srv, ch);
    const ChanState *got = axl_ssh_channel_user_data(srv, ch);
    if (pr != principal) {
        axl_printf("SERVER: PRINCIPAL-MISMATCH\n");
    }
    if (prc == AXL_OK) {
        axl_printf("SERVER: CHANNEL-OPEN %u %s peer=%u.%u.%u.%u:%u"
                   " principal=%d ud=%u\n", (unsigned)ch,
                   u != NULL ? u : "(null)", a[0], a[1], a[2], a[3],
                   (unsigned)port, pr != NULL, got != NULL ? got->n : 0u);
    } else {
        axl_printf("SERVER: CHANNEL-OPEN %u %s peer=? rc=%d principal=%d"
                   " ud=%u\n", (unsigned)ch, u != NULL ? u : "(null)", prc,
                   pr != NULL, got != NULL ? got->n : 0u);
    }
    /* Acting on the channel from INSIDE the open hook, after accepting: the
       header's order is OPEN_CONFIRMATION first, then whatever these did. */
    if (axl_strcmp(user, "openwrite") == 0) {
        static const char OPENED[] = "opened\n";
        axl_printf("SERVER: OPEN-WRITE %d\n",
                   axl_ssh_channel_write(srv, ch, OPENED,
                                         sizeof OPENED - 1));
    } else if (axl_strcmp(user, "openclose") == 0) {
        axl_printf("SERVER: OPEN-CLOSE %d\n",
                   axl_ssh_channel_close(srv, ch));
    } else if (axl_strcmp(user, "openexit") == 0) {
        axl_printf("SERVER: OPEN-EXIT %d\n",
                   axl_ssh_channel_exit(srv, ch, 0));
    }
    return AXL_OK;
}

static int
chan_on_pty(AxlSshChannel ch, const AxlSshPty *pty, void *ud)
{
    ChanState *st = chan_enter(axl_ssh_channel_user_data((AxlSshServer *)ud,
                                                         ch));

    axl_printf("SERVER: PTY %s %ux%u\n", pty->term, (unsigned)pty->cols,
               (unsigned)pty->rows);
    chan_leave(st);
    return AXL_OK;
}

static void
chan_on_resize(AxlSshChannel ch, const AxlSshPty *size, void *ud)
{
    ChanState *st = chan_enter(axl_ssh_channel_user_data((AxlSshServer *)ud,
                                                         ch));

    axl_printf("SERVER: RESIZE %ux%u\n", (unsigned)size->cols,
               (unsigned)size->rows);
    chan_leave(st);
}

static int
chan_on_shell(AxlSshChannel ch, void *ud)
{
    ChanState *st = chan_enter(axl_ssh_channel_user_data((AxlSshServer *)ud,
                                                         ch));

    if (st == NULL) {
        return AXL_ERR;
    }
    st->shell   = true;
    st->started = true;
    chan_leave(st);
    return AXL_OK;
}

/* "hoard N ACT": N, and ACT one of all|split|arrival|none|inline. */
static bool
chan_hoard_parse(ChanState *st, const char *arg)
{
    static const struct {
        const char *name;
        char        act;
    } acts[] = {
        { "all", 'a' }, { "split", 's' }, { "arrival", 'r' },
        { "none", 'n' }, { "inline", 'i' },
    };
    char   **f  = axl_strsplit(arg, ' ');
    uint32_t n  = 0;
    bool     ok = f != NULL && f[0] != NULL && f[1] != NULL && f[2] == NULL
                  && axl_str_to_u32(f[0], 10, &n, NULL) == AXL_OK;

    st->hoard_act = 0;
    for (size_t i = 0; ok && i < AXL_ARRAY_SIZE(acts); i++) {
        if (axl_strcmp(f[1], acts[i].name) == 0) {
            st->hoard_act = acts[i].act;
        }
    }
    axl_strfreev(f);
    st->hoard_at = n;
    return ok && st->hoard_act != 0;
}

/* "hoard" from inside the exec hook: ON_CONSUME before any byte can arrive,
   and every refusal the window calls make on a live channel. */
static void
chan_hoard_start(AxlSshServer *srv, ChanState *st)
{
    size_t idle = 99;

    st->sink = axl_checksum_new(AXL_CHECKSUM_SHA256);
    if (st->sink == NULL) {
        axl_printf("SERVER: FAIL sink\n");
        chan_exit(srv, st->ch, 1);
        return;
    }
    int mode = axl_ssh_channel_set_window_mode(
                   srv, st->ch, AXL_SSH_CHANNEL_WINDOW_ON_CONSUME);
    int same = axl_ssh_channel_set_window_mode(
                   srv, st->ch, AXL_SSH_CHANNEL_WINDOW_ON_CONSUME);
    int bad  = axl_ssh_channel_set_window_mode(srv, st->ch,
                                               (AxlSshChannelWindowMode)7);
    int irc  = axl_ssh_channel_unconsumed(srv, st->ch, &idle);
    int nul  = axl_ssh_channel_unconsumed(srv, st->ch, NULL);
    int zero = axl_ssh_channel_consumed(srv, st->ch, 0);
    int one  = axl_ssh_channel_consumed(srv, st->ch, 1);
    axl_printf("SERVER: HOARD mode=%d same=%d bad=%d idle=%d,%zu nullout=%d"
               " zero=%d one=%d\n", mode, same, bad, irc, idle, nul, zero,
               one);
}

/* The exec table (see THE FIXTURE SHELL). */
static void
chan_exec_cmd(AxlSshServer *srv, ChanState *st, const char *cmd, size_t len)
{
    AxlSshChannel ch = st->ch;
    uint32_t      v  = 0;

    if (len >= 5 && axl_strncmp(cmd, "echo ", 5) == 0) {
        chan_write(srv, ch, false, cmd + 5, len - 5);
        chan_write(srv, ch, false, "\n", 1);
        chan_exit(srv, ch, 0);
    } else if (len >= 4 && axl_strncmp(cmd, "err ", 4) == 0) {
        chan_write(srv, ch, true, cmd + 4, len - 4);
        chan_write(srv, ch, true, "\n", 1);
        chan_exit(srv, ch, 3);
    } else if (axl_strcmp(cmd, "true") == 0) {
        chan_exit(srv, ch, 0);
    } else if (axl_strcmp(cmd, "false") == 0) {
        chan_exit(srv, ch, 1);
    } else if (len >= 5 && axl_strncmp(cmd, "hold ", 5) == 0
               && axl_str_to_u32(cmd + 5, 10, &v, NULL) == AXL_OK && v > 0) {
        if (axl_loop_add_timeout(g_loop, v, on_hold_done,
                                 (void *)(uintptr_t)ch) == 0) {
            axl_printf("SERVER: FAIL hold-timer\n");
            chan_exit(srv, ch, 1);
        }
    } else if (len >= 5 && axl_strncmp(cmd, "blob ", 5) == 0
               && axl_str_to_u32(cmd + 5, 10, &v, NULL) == AXL_OK && v > 0
               && v <= BLOB_MAX) {
        st->blob = v;
        chan_blob(srv, st);
    } else if (len >= 7 && axl_strncmp(cmd, "stream ", 7) == 0
               && axl_str_to_u32(cmd + 7, 10, &v, NULL) == AXL_OK && v > 0) {
        st->stream_left = v;
        st->stream_done = 0;
        chan_stream(srv, st);
    } else if (axl_strcmp(cmd, "cat") == 0) {
        st->cat = true;
    } else if (axl_strcmp(cmd, "sink") == 0) {
        st->sink = axl_checksum_new(AXL_CHECKSUM_SHA256);
        if (st->sink == NULL) {
            axl_printf("SERVER: FAIL sink\n");
            chan_exit(srv, ch, 1);
        }
    } else if (len >= 7 && axl_strncmp(cmd, "adjoom ", 7) == 0
               && axl_str_to_u32(cmd + 7, 10, &v, NULL) == AXL_OK && v > 0) {
        st->sink = axl_checksum_new(AXL_CHECKSUM_SHA256);
        if (st->sink == NULL) {
            axl_printf("SERVER: FAIL sink\n");
            chan_exit(srv, ch, 1);
            return;
        }
        st->adjoom_at = v;
    } else if (len >= 6 && axl_strncmp(cmd, "hoard ", 6) == 0
               && chan_hoard_parse(st, cmd + 6)) {
        chan_hoard_start(srv, st);
    } else if (axl_strcmp(cmd, "sinkflood") == 0) {
        st->sink = axl_checksum_new(AXL_CHECKSUM_SHA256);
        if (st->sink == NULL) {
            axl_printf("SERVER: FAIL sink\n");
            chan_exit(srv, ch, 1);
            return;
        }
        st->slow       = true;
        st->flood_left = SINK_BYTES;
        st->flood_done = 0;
        chan_flood(srv, st);
    } else {
        static const char UNKNOWN[] = "unknown command\n";
        chan_write(srv, ch, true, UNKNOWN, sizeof UNKNOWN - 1);
        chan_exit(srv, ch, 127);
    }
}

/* The subsystem hook: prints SERVER: SUBSYSTEM <name> <len> -- the name as
   a C string, so a name[len] that is not NUL shows as trailing garbage --
   and accepts only "cat", which then copies data back as exec "cat" does. */
static int
chan_on_subsystem(AxlSshChannel ch, const char *name, size_t len, void *ud)
{
    AxlSshServer *srv = ud;
    ChanState    *st  = chan_enter(axl_ssh_channel_user_data(srv, ch));

    axl_printf("SERVER: SUBSYSTEM %s %u\n", name, (unsigned)len);
    if (st == NULL) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (len == 3 && axl_strcmp(name, "cat") == 0) {
        st->started = true;
        st->cat     = true;
        rc          = AXL_OK;
    }
    chan_leave(st);
    return rc;
}

static int
chan_on_exec(AxlSshChannel ch, const char *cmd, size_t len, void *ud)
{
    AxlSshServer *srv = ud;
    ChanState    *st  = chan_enter(axl_ssh_channel_user_data(srv, ch));

    if (st == NULL) {
        return AXL_ERR;
    }
    st->started = true;
    chan_exec_cmd(srv, st, cmd, len);
    chan_leave(st);
    return AXL_OK;
}

/* Run one complete shell line. False when the channel is finished and the
   rest of the input must be ignored. */
static bool
chan_shell_line(AxlSshServer *srv, ChanState *st)
{
    const char   *l  = st->line;
    AxlSshChannel ch = st->ch;
    uint32_t      v  = 0;

    if (axl_strncmp(l, "echo ", 5) == 0) {
        chan_write(srv, ch, false, l + 5, st->line_len - 5);
        chan_write(srv, ch, false, "\r\n", 2);
    } else if (axl_strncmp(l, "stderr ", 7) == 0) {
        chan_write(srv, ch, true, l + 7, st->line_len - 7);
        chan_write(srv, ch, true, "\r\n", 2);
    } else if (axl_strncmp(l, "exit ", 5) == 0
               && axl_str_to_u32(l + 5, 10, &v, NULL) == AXL_OK) {
        chan_exit(srv, ch, v);
        return false;
    } else if (axl_strcmp(l, "quit") == 0) {
        axl_printf("SERVER: QUIT %d\n", axl_ssh_channel_close(srv, ch));
        return false;
    } else if ((axl_strncmp(l, "flood ", 6) == 0
                && axl_str_to_u32(l + 6, 10, &v, NULL) == AXL_OK)
               || (axl_strncmp(l, "slowflood ", 10) == 0
                   && axl_str_to_u32(l + 10, 10, &v, NULL) == AXL_OK)
               || (axl_strncmp(l, "holdflood ", 10) == 0
                   && axl_str_to_u32(l + 10, 10, &v, NULL) == AXL_OK)) {
        st->slow       = l[0] == 's';
        st->hold       = l[0] == 'h';
        st->flood_left = v;
        st->flood_done = 0;
        chan_flood(srv, st);
    } else if (axl_strncmp(l, "pause ", 6) == 0
               && axl_str_to_u32(l + 6, 10, &v, NULL) == AXL_OK) {
        chan_dispatch((uint64_t)v * 1000u);
    } else if (axl_strcmp(l, "holdpause") == 0) {
        chan_await_hold();
    } else if (axl_strcmp(l, "mem") == 0) {
        AxlMemStats ms;
        axl_mem_get_stats(&ms);
        axl_printf("SERVER: MEM %zu %zu\n", ms.count, ms.bytes);
    } else if (axl_strcmp(l, "closeoom") == 0) {
        if (axl_loop_add_timeout(g_loop, 50, on_close_oom,
                                 (void *)(uintptr_t)ch) == 0) {
            axl_printf("SERVER: FAIL closeoom-timer\n");
        }
        return false;
    } else if (axl_strcmp(l, "freeserver") == 0) {
        /* The server goes; this frame keeps only its ADDRESS and the
           handle, and the write below must be refused without the server
           being read. The close hook for this channel must come AFTER this
           hook returns -- after both lines below. */
        axl_ssh_server_free(srv);
        g_srv       = NULL;
        g_srv_freed = true;
        axl_printf("SERVER: FREE-RETURNED\n");
        axl_printf("SERVER: AFTER-FREE %d\n",
                   axl_ssh_channel_write(srv, ch, "x", 1));
        return false;
    } else if (st->line_len > 0) {
        static const char UNKNOWN[] = "unknown command\r\n";
        chan_write(srv, ch, false, UNKNOWN, sizeof UNKNOWN - 1);
    }
    return true;
}

/* The line shell over one data hook's bytes. Reads `buf` to the end even
   across a "pause", which dispatches the loop in the middle of it: the
   library promises the buffer is not touched while the hook holds it. */
static void
chan_shell_input(AxlSshServer *srv, ChanState *st, const uint8_t *buf,
                 size_t len)
{
    AxlSshChannel ch = st->ch;

    for (size_t i = 0; i < len; i++) {
        char b = (char)buf[i];
        if (b == '\n' && st->last_cr) {
            st->last_cr = false;
            continue;
        }
        st->last_cr = b == '\r';
        if (b != '\r' && b != '\n') {
            chan_write(srv, ch, false, &b, 1);
            if (st->line_len + 1 < sizeof st->line) {
                st->line[st->line_len++] = b;
            }
            continue;
        }
        chan_write(srv, ch, false, "\r\n", 2);
        st->line[st->line_len] = '\0';
        bool more = chan_shell_line(srv, st);
        st->line_len = 0;
        if (!more) {
            /* st is still ours: the close hook that frees it never runs
               inside this hook, whatever the command did. */
            st->ended = true;
            return;
        }
    }
}

/* "hoard"'s timer: the window calls an application makes from the loop,
   long after the data hook that was handed the bytes. */
static bool
on_hoard_timer(void *data)
{
    AxlSshChannel ch = (AxlSshChannel)(uintptr_t)data;
    ChanState    *st = axl_ssh_channel_user_data(g_chan_srv, ch);
    size_t        u  = 0;
    size_t        after = 0;

    if (st == NULL) {
        axl_printf("SERVER: HOARD-TIMER gone\n");
        return AXL_SOURCE_REMOVE;
    }
    st->hoard_armed = false;
    int urc  = axl_ssh_channel_unconsumed(g_chan_srv, ch, &u);
    int over = axl_ssh_channel_consumed(g_chan_srv, ch, u + 1u);
    int huge = axl_ssh_channel_consumed(g_chan_srv, ch, SIZE_MAX);
    int act  = AXL_OK;
    if (st->hoard_act == 'a') {
        act = axl_ssh_channel_consumed(g_chan_srv, ch, u);
    } else if (st->hoard_act == 's') {
        size_t left = u;
        for (unsigned i = 0; i < HOARD_SPLIT && act == AXL_OK; i++) {
            size_t n = i + 1u == HOARD_SPLIT ? left : u / HOARD_SPLIT;
            act   = axl_ssh_channel_consumed(g_chan_srv, ch, n);
            left -= n;
        }
    } else if (st->hoard_act == 'r') {
        act = axl_ssh_channel_set_window_mode(
                  g_chan_srv, ch, AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL);
    }
    int arc = axl_ssh_channel_unconsumed(g_chan_srv, ch, &after);
    axl_printf("SERVER: HOARD-TIMER unconsumed=%d,%zu over=%d huge=%d"
               " act=%d after=%d,%zu\n", urc, u, over, huge, act, arc, after);
    return AXL_SOURCE_REMOVE;
}

/* "hoard" after its data hook counted @a len more bytes: report them at
   once ("inline"), or arm the timer once N are held. */
static void
chan_hoard_data(AxlSshServer *srv, ChanState *st, size_t len)
{
    size_t u = 0;

    if (st->hoard_act == 0) {
        return;
    }
    if (st->hoard_act == 'i') {
        int rc = axl_ssh_channel_consumed(srv, st->ch, len);
        if (rc != AXL_OK) {
            axl_printf("SERVER: HOARD-INLINE %d\n", rc);
        }
        return;
    }
    if (st->hoard_at == 0 || st->hoard_armed
        || axl_ssh_channel_unconsumed(srv, st->ch, &u) != AXL_OK
        || u < st->hoard_at) {
        return;
    }
    if (axl_loop_add_timeout(g_loop, HOARD_MS, on_hoard_timer,
                             (void *)(uintptr_t)st->ch) == 0) {
        axl_printf("SERVER: FAIL hoard-timer\n");
        return;
    }
    st->hoard_armed = true;
}

static void
chan_on_data(AxlSshChannel ch, const uint8_t *buf, size_t len, void *ud)
{
    AxlSshServer *srv = ud;
    ChanState    *st  = chan_enter(axl_ssh_channel_user_data(srv, ch));

    if (st == NULL) {
        axl_printf("SERVER: DATA-NO-STATE %zu\n", len);
        return;
    }
    if (!st->started) {
        axl_printf("SERVER: DATA-BEFORE-START %zu\n", len);
    }
    if (st->eof) {
        axl_printf("SERVER: DATA-AFTER-EOF %zu\n", len);
    }
    if (st->sink != NULL) {
        axl_checksum_update(st->sink, buf, len);
        st->sink_bytes += len;
        chan_hoard_data(srv, st, len);
        if (st->adjoom_at != 0 && st->sink_bytes == st->adjoom_at) {
            /* Printed first: the console may allocate. The scenario puts
               the next packet in the SAME receive as this one, so nothing
               receiving it can take the rigged allocation: the next one is
               that packet's parse. */
            st->adjoom_at = 0;
            /* The allocator names the allocation it fails at DEBUG, in its
               own domain: that line is how the scenario knows WHICH one
               took the failure. */
            axl_log_set_domain_level("mem", AXL_LOG_DEBUG);
            axl_printf("SERVER: ADJOOM armed\n");
            /* The SECOND: decrypting the next packet takes one allocation
               (axl_ssh_aead_open's cleartext) before its parse builds the
               adjust. */
            axl_mem_fail_next_alloc(2);
        }
    } else if (st->cat) {
        chan_write(srv, ch, false, buf, len);
    } else if (st->shell && !st->ended) {
        chan_shell_input(srv, st, buf, len);
    }
    chan_leave(st);
}

static void
chan_on_eof(AxlSshChannel ch, void *ud)
{
    AxlSshServer *srv = ud;
    ChanState    *st  = chan_enter(axl_ssh_channel_user_data(srv, ch));

    axl_printf("SERVER: EOF\n");
    if (st != NULL) {
        st->eof = true;
    }
    if (st != NULL && st->cat) {
        chan_exit(srv, ch, 0);
    }
    if (st != NULL && st->sink != NULL) {
        if (st->hoard_act != 0) {
            size_t u   = 0;
            int    urc = axl_ssh_channel_unconsumed(srv, ch, &u);
            axl_printf("SERVER: HOARD-EOF unconsumed=%d,%zu\n", urc, u);
        }
        st->sink_eof = true;
        chan_sink_finish(srv, st);
    }
    chan_leave(st);
}

static void
chan_on_writable(AxlSshChannel ch, void *ud)
{
    AxlSshServer *srv = ud;
    ChanState    *st  = chan_enter(axl_ssh_channel_user_data(srv, ch));

    axl_printf("SERVER: WRITABLE\n");
    if (st != NULL && st->blob > 0) {
        chan_blob(srv, st);
    } else if (st != NULL && st->stream_left > 0) {
        chan_stream(srv, st);
    } else if (st != NULL && st->flood_left > 0) {
        if (st->slow) {
            chan_dispatch(SLOW_US);
        } else if (st->hold) {
            st->hold = false;
            chan_await_hold();
        }
        chan_flood(srv, st);
    } else if (st != NULL) {
        chan_sink_finish(srv, st);
    }
    chan_leave(st);
}

static void
chan_on_close(AxlSshChannel ch, void *chan_data, void *ud)
{
    ChanState *st = chan_data;

    /* The close hook of a channel must never run under another of its
       hooks: such a hook would still be holding st. */
    if (st != NULL && st->depth > 0) {
        axl_printf("SERVER: HOOK-NESTED\n");
    }
    axl_printf("SERVER: CHANNEL-CLOSE ud=%u\n", st != NULL ? st->n : 0u);
    /* The handle is retired before this runs: even from inside the hook. */
    axl_printf("SERVER: CLOSE-LOOKUP %d\n",
               axl_ssh_channel_set_user_data((AxlSshServer *)ud, ch, NULL));
    if (g_stale && g_loop_running
        && axl_loop_add_timeout(g_loop, 200, on_stale_write,
                                (void *)(uintptr_t)ch) == 0) {
        axl_printf("SERVER: FAIL stale-timer\n");
    }
    if (st != NULL) {
        axl_checksum_free(st->sink);
    }
    axl_free(st);
}

static const AxlSshChannelOps g_chan_ops = {
    .open      = chan_on_open,
    .pty       = chan_on_pty,
    .shell     = chan_on_shell,
    .exec      = chan_on_exec,
    .subsystem = chan_on_subsystem,
    .resize    = chan_on_resize,
    .data      = chan_on_data,
    .eof       = chan_on_eof,
    .writable  = chan_on_writable,
    .close     = chan_on_close,
};

/* The extra servers' hooks: the same, but with NO subsystem hook, so a
   suite can show a server without one refusing `subsystem` as before. */
static const AxlSshChannelOps g_chan_ops_nosub = {
    .open      = chan_on_open,
    .pty       = chan_on_pty,
    .shell     = chan_on_shell,
    .exec      = chan_on_exec,
    .resize    = chan_on_resize,
    .data      = chan_on_data,
    .eof       = chan_on_eof,
    .writable  = chan_on_writable,
    .close     = chan_on_close,
};

static bool
on_deadline(void *data)
{
    axl_printf("SERVER: DEADLINE\n");
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

/* THE EARLY FINISH's timer: the deadline's twin, armed by the host's
   signal. */
static bool
on_finish(void *data)
{
    axl_printf("SERVER: FINISH\n");
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

/* The host's "every scenario is over": arm the finish timer, once. The
   connection carries nothing -- that it arrived is the whole message -- so
   it is reset at once, and the callback asks for no further accepts (a
   second signal could only re-arm what is already armed). The library
   still re-arms the accept itself after some of ITS failures (a failed
   protocol lookup or allocation), so SERVER: CTL-FAIL is a report, not an
   ending. */
static bool
on_ctl_accept(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    if (status != AXL_OK || sock == NULL) {
        axl_printf("SERVER: CTL-FAIL %d\n", (int)status);
        return false;
    }
    axl_tcp_close(sock, AXL_TEARDOWN_RESET);
    if (axl_loop_add_timeout((AxlLoop *)data, FINISH_MS, on_finish, data)
        == 0) {
        axl_printf("SERVER: FAIL finish-timer\n");
        return false;
    }
    axl_printf("SERVER: FINISH-ARMED\n");
    return false;
}

/* A heartbeat every 2 s, so a missing SERVER: DONE can distinguish "the loop
   is stuck inside one callback" (ticks stop) from "the loop is running but
   the deadline source never fires" (ticks continue). Without it the two look
   identical from the host, which cost three wrong diagnoses.

   2 s rather than 5 s so the host can sample a meaningful window quickly: the
   host's check must finish well before the deadline quits the loop, because
   after that the ticks legitimately stop and a slow check reads that as a
   stall. That race produced two false failures before the margins were
   widened. */
static bool
on_tick(void *data)
{
    (void)data;
    axl_printf("SERVER: TICK %u\n", ++tick_count);
    return AXL_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
    uint32_t port            = PORT_DEFAULT;
    uint32_t deadline_ms     = DEADLINE_MS_DEFAULT;
    uint32_t login_grace_sec = 0;   /* 0 means "argument not given" */
    /* Positional arguments below this index, name=value options at and above
       it. Every `nargs >` guard below therefore reads "this POSITIONAL slot
       was given", which is what it always meant. */
    const int nargs = count_positional(argc, argv);

    if (nargs > 1 && axl_str_to_u32(argv[1], 10, &port, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL port-arg\n");
        return 1;
    }
    if (nargs > 2
        && axl_str_to_u32(argv[2], 10, &deadline_ms, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL deadline-arg\n");
        return 1;
    }
    if (nargs > 5
        && axl_str_to_u32(argv[5], 10, &login_grace_sec, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL grace-arg\n");
        return 1;
    }
    /* Task P. The penalty spec is parsed HERE, before anything exists, so a
       malformed one fails before a single server is created; "-" leaves the
       shipped defaults in force. */
    AxlSshPenalties pen      = { 0, 0, 0, 0, 0 };
    bool            have_pen = false;
    uint32_t        port2    = 0;
    uint32_t        grace2   = 0;
    if (nargs > 6 && axl_strcmp(argv[6], "-") != 0) {
        if (!parse_penalties(argv[6], &pen)) {
            axl_printf("SERVER: FAIL penalties\n");
            return 1;
        }
        have_pen = true;
    }
    if (nargs > 8 && axl_str_to_u32(argv[8], 10, &port2, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL port2-arg\n");
        return 1;
    }
    if (nargs > 9 && axl_str_to_u32(argv[9], 10, &grace2, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL grace2-arg\n");
        return 1;
    }
    /* The name=value options, parsed HERE for the reason the penalty spec is
       -- before a server exists, so a bad one fails before anything is set
       up. An unknown name is a HARD failure: a typo that selected nothing
       would boot a server configured differently from what the test
       believes and pass whatever it then measured. count_positional stops
       at the first '=', not at the first KNOWN name, which is what puts a
       misspelling in reach of this loop at all -- see its comment. */
    for (int i = nargs; i < argc; i++) {
        /* All of them asked up front rather than one per `else if` arm:
           `(v = opt_value(...)) != NULL` inside an `if` condition is what
           bugprone-assignment-in-if-condition rejects, and src/, tools/ and
           test/unit hold not one instance of it. Safe because opt_value has
           no side effects and, by its own `arg[n] == '='` test, matches at
           most one of these names for a given argument -- so asking them
           all and testing the results in the same order is the same
           decision, for a handful of axl_strncmp on a string already in
           hand. */
        const char *v_maxpwfail   = opt_value(argv[i], "maxpwfail");
        const char *v_maxstartups = opt_value(argv[i], "maxstartups");
        const char *v_latency     = opt_value(argv[i], "latency");
        const char *v_capafter    = opt_value(argv[i], "capafter");
        const char *v_banner      = opt_value(argv[i], "banner");
        const char *v_bannerafter = opt_value(argv[i], "bannerafter");
        const char *v_hookbudget  = opt_value(argv[i], "hookbudget");
        const char *v_sshloglevel = opt_value(argv[i], "sshloglevel");
        const char *v_holdsend    = opt_value(argv[i], "holdsend");
        const char *v_stale       = opt_value(argv[i], "stale");
        const char *v_writelimit  = opt_value(argv[i], "writelimit");
        const char *v_ctl         = opt_value(argv[i], "ctl");
        /* Every per-server option (g_srv_opt_names), parsed by one table
           walk; "" = parsed, another string = the kind that failed. */
        const char *v_srv         = parse_srv_opt(argv[i]);

        if (v_srv != NULL) {
            if (*v_srv != '\0') {
                axl_printf("SERVER: FAIL %s-arg\n", v_srv);
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_ctl != NULL) {
            if (axl_str_to_u32(v_ctl, 10, &g_ctl_port, NULL) != AXL_OK
                || g_ctl_port == 0 || g_ctl_port > 65535u) {
                axl_printf("SERVER: FAIL ctl-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_writelimit != NULL) {
            if (!parse_write_limit(v_writelimit)) {
                axl_printf("SERVER: FAIL writelimit-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_stale != NULL) {
            if (axl_strcmp(v_stale, "1") != 0) {
                axl_printf("SERVER: FAIL stale-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
            g_stale = true;
        } else if (v_holdsend != NULL) {
            if (axl_str_to_u32(v_holdsend, 10, &g_hold_send, NULL) != AXL_OK) {
                axl_printf("SERVER: FAIL holdsend-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_sshloglevel != NULL) {
            if (axl_strcmp(v_sshloglevel, "debug") == 0) {
                g_ssh_log_level = AXL_LOG_DEBUG;
            } else if (axl_strcmp(v_sshloglevel, "info") == 0) {
                g_ssh_log_level = AXL_LOG_INFO;
            } else {
                axl_printf("SERVER: FAIL sshloglevel-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_hookbudget != NULL) {
            if (axl_str_to_u32(v_hookbudget, 10, &g_hook_budget_ms, NULL)
                != AXL_OK) {
                axl_printf("SERVER: FAIL hookbudget-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_maxpwfail != NULL) {
            if (axl_str_to_u32(v_maxpwfail, 10, &g_max_pw_fail, NULL) != AXL_OK) {
                axl_printf("SERVER: FAIL maxpwfail-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_latency != NULL) {
            AxlNetLatency lat   = { 0 };   /* AXL_NET_LATENCY_FIRMWARE */
            bool          known = true;
            if (axl_strcmp(v_latency, "window") == 0) {
                lat.mode = AXL_NET_LATENCY_WINDOW;
            } else if (axl_strcmp(v_latency, "always") == 0) {
                lat.mode = AXL_NET_LATENCY_ALWAYS;
            } else {
                known = axl_strcmp(v_latency, "firmware") == 0;
            }
            if (!known || axl_net_set_latency(&lat) != AXL_OK) {
                axl_printf("SERVER: FAIL latency-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_maxstartups != NULL) {
            if (axl_str_to_u32(v_maxstartups, 10, &g_max_startups, NULL)
                != AXL_OK) {
                axl_printf("SERVER: FAIL maxstartups-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_capafter != NULL) {
            if (axl_str_to_u32(v_capafter, 10, &g_cap_after, NULL) != AXL_OK) {
                axl_printf("SERVER: FAIL capafter-arg\n");
                axl_free(g_banner);
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_banner != NULL) {
            /* Freed first: an option given TWICE would otherwise drop the
               earlier load. */
            axl_free(g_banner);
            g_banner = load_text(v_banner);
            if (g_banner == NULL) {
                axl_printf("SERVER: FAIL banner-file\n");
                axl_free(g_banner_after);
                return 1;
            }
        } else if (v_bannerafter != NULL) {
            axl_free(g_banner_after);
            g_banner_after = load_text(v_bannerafter);
            if (g_banner_after == NULL) {
                axl_printf("SERVER: FAIL bannerafter-file\n");
                axl_free(g_banner);
                return 1;
            }
        } else {
            axl_printf("SERVER: FAIL unknown-option\n");
            axl_free(g_banner);
            axl_free(g_banner_after);
            return 1;
        }
    }

    /* BEFORE any of the setup below can log: the level must be in force
       for the whole boot, not from the first connection onward. */
    if (g_ssh_log_level >= 0) {
        axl_log_set_domain_level("ssh", g_ssh_log_level);
    }
    for (unsigned i = 0; i < SERVERS; i++) {
        g_channels = g_channels || g_srv_opts[i].channels;
    }
    if (g_channels && axl_log_add_handler(on_ssh_log, NULL) != AXL_OK) {
        axl_printf("SERVER: FAIL log-handler\n");
        return 1;
    }
    /* Armed before anything can send, so N counts from this boot's first
       TCP send retirement; 0 leaves it disarmed. The seam reports firing at
       DEBUG in the "tcp" domain, so that domain is raised with it -- a test
       has to be able to see that the hold it asked for happened. */
    axl_tcp_fail_next_send_schedule(g_hold_send);
    if (g_hold_send > 0) {
        axl_log_set_domain_level("tcp", AXL_LOG_DEBUG);
    }

    /* The startup script's DHCP is enough on x64/OVMF but NOT on aa64/AAVMF,
       where axl_tcp_listen finds no TCP4 service binding without this. Same
       call 9p-server-selftest makes, for the same measured reason. */
    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);

    AxlLoop *loop = axl_loop_new();
    g_loop        = loop;
    if (loop == NULL) {
        axl_printf("SERVER: FAIL loop\n");
        return 1;
    }

    AxlSshServer *srv   = NULL;
    const char   *stage = NULL;

    /* NULL here most likely means the image did not link the Ed25519
       provider -- the host key algorithm is ssh-ed25519 and there is no
       fallback, so this is a build error surfacing at runtime. */
    srv        = axl_ssh_server_new(loop, (uint16_t)port);
    g_srv      = srv;
    g_chan_srv = srv;
    axl_memset(g_flood_bytes, 'x', sizeof g_flood_bytes);
    axl_memset(g_slow_bytes, '.', sizeof g_slow_bytes);
    axl_memset(g_blob_bytes, 'b', sizeof g_blob_bytes);
    for (size_t i = 0; i < sizeof g_stream_bytes; i++) {
        g_stream_bytes[i] = (char)(i % 251u);
    }
    if (srv == NULL) {
        stage = "server-new";
    /* The keys come from a file on the ESP rather than a constant in this
       fixture: the host generates a throwaway pair per run and stages the
       PUBLIC half, so no private key is ever committed and the loader itself
       is exercised end to end instead of being stubbed out. "-" means "load
       none" -- P3's password-only scenarios pass it. */
    } else if (nargs > 3 && axl_strcmp(argv[3], "-") != 0
               && axl_ssh_server_load_authorized_keys(srv, argv[3])
                      != AXL_OK) {
        stage = "authkeys";
    } else if (nargs > 4 && !load_creds(argv[4])) {
        stage = "creds";
    } else if (nargs > 4
               && axl_ssh_server_set_auth_ops(
                      srv,
                      &(AxlSshAuthOps){ on_password, on_publickey, on_release },
                      NULL) != AXL_OK) {
        stage = "auth-ops";
    } else if (nargs > 4
               && axl_loop_add_timer(loop, TICK_MS, on_complete_tick, NULL)
                      == 0) {
        stage = "complete-tick";
    } else if (nargs > 5
               && axl_ssh_server_set_login_grace(srv, login_grace_sec)
                      != AXL_OK) {
        stage = "grace";
    } else if (g_max_pw_fail != 0
               && axl_ssh_server_set_max_password_failures(srv, g_max_pw_fail)
                      != AXL_OK) {
        stage = "maxpwfail";
    } else if (g_max_startups != 0
               && axl_ssh_server_set_max_startups(srv, g_max_startups)
                      != AXL_OK) {
        stage = "maxstartups";
    } else if (g_hook_budget_ms != UINT32_MAX
               && axl_ssh_server_set_auth_hook_budget_ms(srv, g_hook_budget_ms)
                      != AXL_OK) {
        stage = "hookbudget";
    } else if (g_banner != NULL
               && axl_ssh_server_set_banner(srv, g_banner) != AXL_OK) {
        stage = "banner";
    } else if (g_srv_opts[0].channels
               && axl_ssh_server_set_channel_ops(srv, &g_chan_ops, srv)
                      != AXL_OK) {
        stage = "channel-ops";
    } else if (g_write_total != 0
               && axl_ssh_server_set_channel_write_limit(srv, g_write_per,
                                                         g_write_total)
                      != AXL_OK) {
        stage = "writelimit";
    } else if (!apply_srv_opts(srv, &g_srv_opts[0], &stage)) {
        /* stage names the setter: unused, alive or rekeylimit. */
    } else if (have_pen && axl_ssh_server_set_penalties(srv, &pen) != AXL_OK) {
        stage = "penalties";
    } else if (nargs > 7 && axl_strcmp(argv[7], "-") != 0
               && !apply_exempt(srv, argv[7])) {
        stage = "exempt";
    } else if (axl_ssh_server_listen(srv) != AXL_OK) {
        stage = "listen";
    }

    /* THE CHAIN CONTINUES HERE, as `stage == NULL &&` rather than `else if`.
       The extra servers are the one step that NAMES its own failing stage
       instead of being named here, so in the chain it read
       `(stage = start_extra_server(...)) != NULL` -- an assignment inside
       an `if` condition, which is what bugprone-assignment-in-if-condition
       rejects and what no other C in this tree does. `stage == NULL` is the
       same short-circuit `else if` gave: nothing below runs once a step has
       failed.

       THE EXTRA SERVERS' POSITION IS LOAD-BEARING, which is why this was
       not simply hoisted above the chain: it must start after the first
       listen() and before the 2000 ms heartbeat timer at the bottom. See
       that timer's comment for what depends on it. (The deadline timeout
       between them is a far-future one-shot; nothing here claims its
       position matters.) */
    /* The second server's port is positional (port2); the others' are
       options (port3=, port4=, port5=). Each is started only when its port
       was given, in that order. */
    g_srv_opts[1].port = port2;
    for (unsigned i = 0; stage == NULL && i < EXTRA_SERVERS; i++) {
        if (g_srv_opts[1u + i].port != 0) {
            stage = start_extra_server(
                i, loop, g_srv_opts[1u + i].port,
                grace2 != 0 ? grace2 : login_grace_sec,
                have_pen ? &pen : NULL,
                nargs > 3 && axl_strcmp(argv[3], "-") != 0 ? argv[3] : NULL,
                &g_chan_ops_nosub);
        }
    }
    if (stage == NULL
        && axl_loop_add_timeout(loop, deadline_ms, on_deadline, loop) == 0) {
        stage = "deadline";
    }
    /* add_timer, NOT add_timeout: the latter is documented one-shot and
       auto-removed, so it ticks exactly once and then looks -- from the host
       -- identical to a loop that has stopped dispatching. That mistake cost
       several runs and one worthless sabotage validation.

       KEEP THIS AFTER EVERY listen() CALL ABOVE. test-ssh-hostile-qemu.sh's
       penalty knocks grade the loop on this heartbeat, and what the check
       really detects is STARVATION: a refusal that stalled after its RST
       leaves the next knock waiting, the re-armed Accept() completes at once,
       and WaitForEvent returns the lowest-index signalled event -- so the
       accept wins every wait only while it sits AHEAD of this timer in the
       loop's source array. Registered ahead of the listeners, the heartbeat
       would win instead, and the check would be blind to any stall under
       its 2 s period (a periodic timer loses no tick to those). "Ahead"
       means array index, not registration time: AxlLoop reuses the first
       free slot, so this also relies on nothing on this loop being removed
       before this line -- true today, since nothing above adds a source it
       later frees -- and on the second server's accept never being
       re-registered during the knocks. The one path that does that is an
       accept paused after MAX_ACCEPT_ERRS consecutive failures (the accept
       source is removed, and the re-arm takes the first free slot, which
       may be after this one); reaching the connection cap no longer pauses
       it (admission by eviction), and nothing in the penalty scenario fails
       an accept. Nothing checks this order at run time; a source's slot is
       not visible through the public API. */
    if (stage == NULL && axl_loop_add_timer(loop, 2000, on_tick, NULL) == 0) {
        stage = "tick";
    }
    /* AFTER the heartbeat, for the reason in its comment: the control
       listener's accept source must not take a slot ahead of it either.
       It signals once, at the very end, so where it sits matters to
       nothing else. */
    if (stage == NULL && g_ctl_port != 0
        && (axl_tcp_listen((uint16_t)g_ctl_port, &g_ctl) != AXL_OK
            || axl_tcp_accept_async(g_ctl, loop, NULL, on_ctl_accept, loop)
                   != AXL_OK)) {
        stage = "ctl";
    }
    if (stage != NULL) {
        axl_printf("SERVER: FAIL %s\n", stage);
        axl_tcp_close(g_ctl, AXL_TEARDOWN_RESET);
        for (unsigned i = 0; i < EXTRA_SERVERS; i++) {
            axl_ssh_server_free(g_srv_extra[i]);
        }
        axl_ssh_server_free(srv);
        axl_loop_free(loop);
        axl_free(g_banner);
        axl_free(g_banner_after);
        return 1;
    }

    /* The harness waits on THIS line, not on anything the library prints:
       a readiness marker is the fixture's output contract, and
       axl_ssh_server_listen already reports success through its return
       value (check-log-levels enforces exactly that distinction). */
    axl_printf("ssh: listening\n");

    g_loop_running = true;
    axl_loop_run(loop);
    g_loop_running = false;

    /* Bracket the teardown, the way 9p-server-selftest does: without this a
       missing SERVER: DONE cannot distinguish "axl_loop_run never returned"
       from "the free hung". It cost a wrong diagnosis once already. */
    axl_printf("SERVER: REAPING\n");
    /* axl_ssh_server_free(srv) here, immediately followed by axl_loop_free,
       is the exact sequence axl_ssh_server_free()'s own header warns a
       consumer away from in general (P3 security re-review NEW-2) -- but it
       is safe at THIS call site specifically, and worth saying why rather
       than just doing it: axl_loop_run() has already returned above, so no
       hook and no release_principal is on the C stack anywhere in this
       process. That means no connection can be auth_in_hook right now, so
       drop_all() defers none of them -- every release_principal this call
       triggers (for whichever connections are still open) runs SYNCHRONOUSLY
       inside it, and app_calls is back to 0 by the time it returns:
       server_finish() frees the struct inline rather than scheduling it. A
       consumer copying this teardown must keep it exactly this shape --
       nothing of the application's between axl_loop_run() returning and this
       call -- or the guarantee above no longer holds. */
    if (!g_srv_freed) {
        axl_ssh_server_free(srv);
    }
    /* The other servers under the same argument: no hook is on the stack
       once axl_loop_run has returned. */
    for (unsigned i = 0; i < EXTRA_SERVERS; i++) {
        axl_ssh_server_free(g_srv_extra[i]);
    }
    /* Its sources live on this loop, so it goes first. */
    axl_tcp_close(g_ctl, AXL_TEARDOWN_RESET);
    axl_loop_free(loop);
    if (g_channels) {
        axl_log_remove_handler(on_ssh_log);
    }
    /* BEFORE the leak verdict below, not after: these are the fixture's own
       allocations, and the server copied whatever it was given. */
    axl_free(g_banner);
    axl_free(g_banner_after);
    axl_printf("SERVER: HOOK-DEPTH-MAX %u\n", g_hook_depth_max);
    /* The heap's own verdict on everything above. A use-after-free WRITE is
       counted only when its block leaves the quarantine (src/mem/axl-mem.c),
       so drain the ring first -- otherwise a write into a block freed in the
       last few frees would still be held, unchecked, when this prints. */
    axl_mem_set_quarantine(0);
    axl_printf("SERVER: MEM-CORRUPTION %zu\n", axl_mem_corruption_count());
    axl_printf("SERVER: DONE\n");
    return 0;
}
