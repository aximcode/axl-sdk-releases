/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-svc.c - the `sshd` service, dual-compiled.
 *
 * With -DAXL_SERVICE_BUILD_DRIVER this file IS sshd-dxe.efi: a resident DXE
 * driver that takes over the running Shell's console (AxlConsoleDevice,
 * passthrough, so the local console keeps painting) and serves it to SSH
 * shell sessions (AxlSshServer). Without it, the file contributes only the
 * descriptor, descs and opts that sshd.c hands to
 * axl_service_start_embedded, so the launcher links neither the server nor
 * the console stack.
 *
 * THE BINDING. AxlSsh knows nothing of consoles and the console stack
 * nothing of SSH; this file is the only place the two meet.
 *
 *   output  console ops -> one coalescing AxlConsoleVtEnc -> flushed from
 *           a 10 ms loop tick -> written to every LIVE shell session, each
 *           flush in ONE all-or-nothing write. A write the channel refuses
 *           (AXL_BUSY) queued nothing and marks that session BEHIND: it is
 *           skipped by the fan-out until it has been repainted from the
 *           encoder's screen model, so a stalled client never holds up the
 *           console or another session. A flush larger than a channel may
 *           hold is sent to no one and every session is repainted instead.
 *           So a session's stream is whole flushes, then a repaint that
 *           starts with a clear: a gap, never a sequence cut in two. A
 *           repaint itself goes out in pieces, but a BEHIND session gets
 *           nothing else until it is all out, so the pieces join up.
 *   join    a new session starts BEHIND, so a late join and a recovery are
 *           the same path: flush what is buffered to everyone else, then
 *           snapshot the screen to the joiner.
 *   input   per-session reassembly (sshd-bind.h) into whole keys, injected
 *           through the device; control letters become Ctrl chords, which
 *           is what the Shell's Ctrl-C (break) and Ctrl-S notifies match.
 *           The console's key ring holds 63 keys and drains only as the
 *           Shell reads, so what does not fit (a paste) waits in a 64 KB
 *           per-session queue, whole runs in order, and is offered again
 *           every tick. AxlSsh cannot push back on a client's input (its
 *           window is an accounting number, not a buffer), so input past
 *           that bound is dropped, counted and logged at DEBUG. Control
 *           keys wait in that queue in order, as a pty keeps them -- the
 *           UEFI editor reads ^S ^Q ^K as commands -- except Ctrl-C, which
 *           goes in at once: its break notify fires even on a full ring, so
 *           it breaks a script behind a paste the script never reads.
 *   EOF     a channel's EOF does not end its session: the client has stopped
 *           typing, not watching, so the session keeps receiving the console
 *           until the channel closes -- as a pty does.
 *   size    the console is never reshaped for a session (other sessions and
 *           the local screen show it too). Each session's terminal size comes
 *           from its pty-req and every window-change. A terminal LARGER than
 *           the console is repainted inside a scroll region of the console's
 *           rows, so the console's scrolling stays in those rows and the rest
 *           stays blank; one SMALLER is shown a notice instead of the console,
 *           and gets no output until it is large enough, when it is
 *           repainted. A terminal that gives no size is taken to match.
 *
 * WHY THE OPS ARE WRAPPED. The encoder appends an op's bytes to its buffer
 * and then feeds them to its screen model, and nothing guards the pair: the
 * console ops run at the Shell's TPL_APPLICATION while the tick runs at
 * TPL_CALLBACK, so a tick can land between the two. The wrapper counts ops
 * in flight, and the tick neither flushes nor snapshots while one is.
 *
 * Every SSH call is made from the loop (the tick and the server's hooks).
 * The console ops never touch a channel: the only way their bytes leave is
 * the tick's flush. The one exception is the encoder flushing on its own
 * when its buffer overflows; that lands inside an op, so the sink drops the
 * bytes and the tick repaints every session instead.
 */

#include <axl.h>

#include "sshd-bind.h"
#include "sshd-sftp-iface.h"
#include "sshd-svc.h"

AXL_LOG_DOMAIN("sshd");

/** The flush/input/repaint tick, and the driver tick that drives the loop:
    a loop timer cannot run faster than the driver tick that dispatches it. */
#define SSHD_TICK_MS          10

/** The most serial ports AxlLogPort chooses among (the enumeration's own
    bound). */
#define SSHD_LOG_PORTS_MAX    64

// ---------------------------------------------------------------------------
// Globals (both images)
// ---------------------------------------------------------------------------

SshdOpts g_sshd_opts;

const AxlConfigDesc sshd_descs[] = {
    { "nic",      AXL_CFG_UINT,   AXL_NET_NIC_AUTO_STR,
      "NIC ordinal to bring up (default: first usable)",
      offsetof(SshdOpts, net.nic_index), sizeof(uint64_t) },
    { "port",     AXL_CFG_UINT,   SSHD_PORT_DEFAULT_STR,
      "TCP port",
      offsetof(SshdOpts, net.port),      sizeof(uint16_t) },
    { "keys",     AXL_CFG_STRING, "",
      "authorized_keys file (absolute path), or empty",
      offsetof(SshdOpts, keys),          sizeof(const char *) },
    { "password", AXL_CFG_STRING, "",
      "accepted password, or empty",
      offsetof(SshdOpts, password),      sizeof(const char *) },
    { "exempt",   AXL_CFG_STRING, "",
      "comma-joined CIDR ranges exempt from login penalties",
      offsetof(SshdOpts, exempt),        sizeof(const char *) },
    { "ro",       AXL_CFG_BOOL,   "false",
      "read-only: session input is dropped",
      offsetof(SshdOpts, ro),            sizeof(bool) },
    { "sftpro",   AXL_CFG_BOOL,   "false",
      "AxlSftpReadOnly: SFTP refuses every write op",
      offsetof(SshdOpts, sftp_ro),       sizeof(bool) },
    { "geometry", AXL_CFG_BOOL,   "false",
      "report each console reshape on the first serial port",
      offsetof(SshdOpts, geometry),      sizeof(bool) },
    { "logport",  AXL_CFG_UINT,   SSHD_LOG_PORT_AUTO_STR,
      "AxlLogPort: the serial port the log goes to while serving",
      offsetof(SshdOpts, log_port),      sizeof(uint64_t) },
    { "sftp",     AXL_CFG_STRING, SFTP_HELPER_NAME,
      "Subsystem sftp: the command, or empty for no SFTP",
      offsetof(SshdOpts, sftp),          sizeof(const char *) },
    { "conf",     AXL_CFG_STRING, "",
      "the settings file's name, or empty when none was read",
      offsetof(SshdOpts, conf),          sizeof(const char *) },
    { "sftppath", AXL_CFG_STRING, "",
      "the SFTP helper's file (absolute path), or empty",
      offsetof(SshdOpts, sftp_path),     sizeof(const char *) },
    { "pubkey",   AXL_CFG_BOOL,   SSHD_PUBKEY_DEFAULT_STR,
      "PubkeyAuthentication",
      offsetof(SshdOpts, pubkey), sizeof(bool) },
    { "pwauth",   AXL_CFG_BOOL,   SSHD_PWAUTH_DEFAULT_STR,
      "PasswordAuthentication",
      offsetof(SshdOpts, pwauth), sizeof(bool) },
    { "grace",    AXL_CFG_UINT,   SSHD_GRACE_DEFAULT_STR,
      "LoginGraceTime, seconds",
      offsetof(SshdOpts, grace_sec), sizeof(uint32_t) },
    { "maxauth",  AXL_CFG_UINT,   SSHD_MAX_AUTH_DEFAULT_STR,
      "MaxAuthTries: wrong passwords per connection",
      offsetof(SshdOpts, max_auth), sizeof(uint32_t) },
    { "maxsess",  AXL_CFG_UINT,   SSHD_MAX_SESS_DEFAULT_STR,
      "MaxSessions",
      offsetof(SshdOpts, max_sessions), sizeof(uint32_t) },
    { "maxstart", AXL_CFG_UINT,   SSHD_MAX_START_DEFAULT_STR,
      "MaxStartups",
      offsetof(SshdOpts, max_startups), sizeof(uint32_t) },
    { "alive",    AXL_CFG_UINT,   SSHD_ALIVE_DEFAULT_STR,
      "ClientAliveInterval, seconds; 0 = off",
      offsetof(SshdOpts, alive_sec), sizeof(uint32_t) },
    { "alivemax", AXL_CFG_UINT,   SSHD_ALIVE_MAX_DEFAULT_STR,
      "ClientAliveCountMax",
      offsetof(SshdOpts, alive_max), sizeof(uint32_t) },
    { "unused",   AXL_CFG_UINT,   SSHD_UNUSED_DEFAULT_STR,
      "UnusedConnectionTimeout, seconds; 0 = none",
      offsetof(SshdOpts, unused_sec), sizeof(uint32_t) },
    { "rekey",    AXL_CFG_UINT,   SSHD_REKEY_DEFAULT_STR,
      "RekeyLimit, bytes per key per direction",
      offsetof(SshdOpts, rekey_bytes), sizeof(uint64_t) },
    { "banner",   AXL_CFG_STRING, "",
      "Banner file path, or empty",
      offsetof(SshdOpts, banner), sizeof(const char *) },
    { "hostkeys", AXL_CFG_STRING, "",
      "HostKey paths, '|'-joined; empty = the stored key",
      offsetof(SshdOpts, host_keys), sizeof(const char *) },
    { "pen",      AXL_CFG_BOOL,   SSHD_PEN_DEFAULT_STR,
      "PerSourcePenalties on",
      offsetof(SshdOpts, penalties), sizeof(bool) },
    { "penauth",  AXL_CFG_UINT,   SSHD_PEN_AUTH_DEFAULT_STR,
      "PerSourcePenalties authfail, seconds",
      offsetof(SshdOpts, pen_authfail), sizeof(uint32_t) },
    { "pennoauth", AXL_CFG_UINT,   SSHD_PEN_NOAUTH_DEFAULT_STR,
      "PerSourcePenalties noauth, seconds",
      offsetof(SshdOpts, pen_noauth), sizeof(uint32_t) },
    { "pengrace", AXL_CFG_UINT,   SSHD_PEN_GRACE_DEFAULT_STR,
      "PerSourcePenalties grace-exceeded, seconds",
      offsetof(SshdOpts, pen_grace), sizeof(uint32_t) },
    { "penmin",   AXL_CFG_UINT,   SSHD_PEN_MIN_DEFAULT_STR,
      "PerSourcePenalties min, seconds",
      offsetof(SshdOpts, pen_min), sizeof(uint32_t) },
    { "penmax",   AXL_CFG_UINT,   SSHD_PEN_MAX_DEFAULT_STR,
      "PerSourcePenalties max, seconds",
      offsetof(SshdOpts, pen_max), sizeof(uint32_t) },
    { "loglevel", AXL_CFG_UINT,   "0",
      "LogLevel (SshdLogLevel); 0 = not given",
      offsetof(SshdOpts, loglevel), sizeof(uint32_t) },
    { "drivers",  AXL_CFG_STRING, "",
      "NIC-driver directory to sweep (absolute), or empty for every volume's",
      offsetof(SshdOpts, drivers), sizeof(const char *) },
    { "nettimeout", AXL_CFG_UINT, SSHD_NET_TIMEOUT_STR,
      "AxlNetworkTimeout: the network bring-up's deadline, seconds",
      offsetof(SshdOpts, net_timeout_sec), sizeof(uint32_t) },
    { "tcplinger", AXL_CFG_UINT, "0",
      "AxlTcpLinger: AxlTcpPacing.linger_us, microseconds; 0 = off",
      offsetof(SshdOpts, tcp_linger_us), sizeof(uint32_t) },
    { "volume",   AXL_CFG_STRING, "",
      "the launcher's volume (FS0:\\), or empty",
      offsetof(SshdOpts, volume), sizeof(const char *) },
    { 0 }
};

void
sshd_addresses(char *out, size_t cap)
{
    AxlNetInterface *ifs = NULL;
    size_t           n   = 0;
    size_t           k   = 0;

    if (axl_net_list_interfaces_alloc(&ifs, &n) != AXL_OK) {
        n = 0;
    }
    char        (*txt)[16] = n > 0 ? axl_calloc(n, sizeof(*txt)) : NULL;
    const char **ips       = n > 0 ? axl_calloc(n, sizeof(*ips)) : NULL;
    for (size_t i = 0; i < n && txt != NULL && ips != NULL; i++) {
        if (ifs[i].has_ipv4 && axl_ipv4_format(ifs[i].ipv4, txt[k], sizeof(txt[k])) == AXL_OK) {
            ips[k] = txt[k];
            k++;
        }
    }
    sshd_bind_addr_list(out, cap, ips, k);
    axl_free(ips);
    axl_free(txt);
    axl_free(ifs);
}

#ifdef AXL_SERVICE_BUILD_DRIVER

#include "sshd-conf.h"
#include "sshd-sftp-relay.h"

// ---------------------------------------------------------------------------
// Driver: macros and types
// ---------------------------------------------------------------------------

/** Bytes one channel may hold queued. About two screens: a session that
    falls this far behind is cheaper to repaint than to replay. */
#define SSHD_CHAN_LIMIT       (16u * 1024u)

/** Largest single write of a repaint. Smaller than the channel limit, so a
    repaint makes partial progress into a queue that is part full. (A live
    flush is never split: see sshd_fan_write().) */
#define SSHD_PIECE            (4u * 1024u)

/** At most one of each repeating DEBUG line (a session fell behind; a release
    waits for a call in flight; a Ctrl-C met a full ring) per this many ms. */
#define SSHD_BEHIND_LOG_MS    1000u

/** How long sessions get to receive the goodbye after the Shell exits,
    before the server is freed under them. */
#define SSHD_DRAIN_MS         3000u

/** How long `sshd stop` drives the loop so its last word reaches the
    sessions before the server is freed under them (abortively: whatever is
    still queued then is lost), and the slice it drives it in. */
#define SSHD_STOP_DRAIN_MS    1000u
#define SSHD_STOP_SLICE_US    20000u

/** The Shell's break and HaltOutput notifies match left Ctrl exactly; the
    toggle state is AXL_CONSOLE_TOGGLE_KEYBOARD, as for every decoded key. */
#define SSHD_CTRL_SHIFT       (AXL_CONSOLE_SHIFT_STATE_VALID \
                               | AXL_CONSOLE_LEFT_CONTROL_PRESSED)

/** The bring-up's link wait, in each pass before its DHCP: long enough for a
    link still settling after its driver connected. */
#define SSHD_LINK_WAIT_SEC    5u

/** The drivers directory's arch component, as the sweep's default names it. */
#if defined(__aarch64__)
#  define SSHD_DRIVERS_ARCH   "aa64"
#else
#  define SSHD_DRIVERS_ARCH   "x64"
#endif

/** sshd-rtk.efi's driver (-DSSHD_EMBEDDED_NIC_DRIVER) carries a NIC driver,
    packed by axl-efi-pack, and its file name: the `sshd-rtk` goal
    (tools/CMakeLists.txt) splices both in (embed blobs sshd_nic_driver,
    sshd_nic_name). The bring-up offers it ahead of the drivers beside
    sshd.efi. */
#ifdef SSHD_EMBEDDED_NIC_DRIVER
AXL_EMBED_DECLARE(sshd_nic_driver);
AXL_EMBED_DECLARE(sshd_nic_name);
#endif

/** What net_up's progress hook keeps, for the message a failure ends with. */
typedef struct {
    uint64_t  until;      ///< the deadline (axl_time_get_ms)
    size_t    tried;      ///< NICs given an address step
    size_t    down;       ///< distinct link-down NICs: the MACs in down_mac
    size_t    down_cap;   ///< room in down_mac, in MACs
    uint8_t (*down_mac)[6]; ///< their MACs, the latest last (heap; NULL until one)
    bool      down_lost;  ///< a link-down NIC could not be recorded (no memory)
} NetProgress;

/** Per-channel state, attached with axl_ssh_channel_set_user_data() and also
    the session table's key. */
typedef struct {
    AxlSshChannel ch;        ///< the server's handle
    char         *rp;        ///< the snapshot a repaint is writing, or NULL
    SshdRepaint   rps;       ///< how far that repaint has got
    SshdWrResult  last_wr;   ///< the repaint's last write, for a failure's cause
    bool          blocked;   ///< refused with AXL_BUSY; waiting for writable
    bool          dead;      ///< refused for good (closing); never retried
    SshdPend      pend;      ///< input the console's key ring could not take yet
    uint32_t      term_cols; ///< the terminal's size (pty-req, window-change); 0 = not given
    uint32_t      term_rows; ///< (rows)
    SshdFit       fit;       ///< how that size fits the console
    bool          region;    ///< a scroll region is set on the terminal
    bool          notice;    ///< held (SMALLER) and the notice is still to be written
    bool          retake;    ///< refitted during a repaint: take a fresh one after it
    SftpRelayChan *sftp;     ///< the channel's SFTP session, or NULL: a channel
                             ///< is a shell or an SFTP session, never both
} SshdChan;

/** One flush's bytes, handed to every live session. */
typedef struct {
    const char *bytes;
    size_t      len;
} FanOut;

/** Where the driver is in its life. */
typedef enum {
    SSHD_RUNNING,    ///< serving the console
    SSHD_DRAINING,   ///< the Shell exited: console released, goodbyes leaving
    SSHD_DONE        ///< server freed; nothing left but unload
} SshdState;

// ---------------------------------------------------------------------------
// Driver: static prototypes
// ---------------------------------------------------------------------------

static int  sshd_setup(AxlLoop *loop, void *user);
static int  sshd_teardown(void *user);

#endif /* AXL_SERVICE_BUILD_DRIVER */

/* Defined in both images. The setup/teardown MEMBERS are gated, not just
   their functions: a launcher-side reference would drag the server and the
   console stack into sshd.efi. The launcher reads only .name, .opts_descs and
   .user (see 9p-serve-svc.c for the same arrangement). */
const AxlService sshd_service = {
    .name           = "sshd",
    .opts_descs     = sshd_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = sshd_setup,
    .teardown       = sshd_teardown,
#endif
    .user           = &g_sshd_opts,
    .driver_tick_ms = SSHD_TICK_MS,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static void enc_sink(const char *bytes, size_t len, void *user);
static void snap_sink(const char *bytes, size_t len, void *user);
static SshdWrResult wr_result(int rc);
static SshdWrResult rp_write(const char *bytes, size_t len, void *ud);
static SshdWrResult fan_write(const char *bytes, size_t len, void *ud);
static void go_behind(SshdChan *c, const char *why);
static void refused_for_good(SshdChan *c, SshdWrResult r);
static void fan_one(SshdSession *s, void *ud);
static void mark_live_behind(SshdSession *s, void *ud);
static void repaint_one(SshdSession *s, void *ud);
static void tick_input(SshdSession *s, void *ud);
static bool inject_ctrl(char letter);
static size_t input_take(SshdInKind kind, const char *bytes, size_t len, void *ud);
static void on_input(SshdInKind kind, const char *bytes, size_t len, void *ud);
static void say_goodbye(SshdSession *s, void *ud);
static bool release_console(void);
static void server_drop(void);
static bool on_tick(void *data);
static void chan_free(SshdChan *c);
static int  chan_on_subsystem(AxlSshChannel ch, const char *name, size_t len, void *ud);
static void chan_on_eof(AxlSshChannel ch, void *ud);
static void chan_refit(SshdChan *c);
static void refit_one(SshdSession *s, void *ud);
static void fit_sync(void);
static void write_notice(SshdChan *c);
static void banner_sync(void);
static void wrap_ops(const AxlConsoleOps *in);
static int  banner_read(const char *path);
static bool refused(const char *keyword, int rc);
static int  server_settings(const SshdOpts *o);
static int  apply_exempt(const char *list);
static int  server_build(SshdOpts *o);
static int  console_take(void);
static int  console_drop(void);
static int  setup_failed(void);
static int  log_port_pick(uint64_t want);
static void log_port_open(bool geometry);
static void kept_handler(int level, const char *domain, const char *message,
                         const AxlRealtime *stamp, void *data);
static void kept_keep(bool on);
static int  net_up(const SshdOpts *o);
static void net_progress(const AxlNetDriverEvent *ev, void *ctx);

// ---------------------------------------------------------------------------
// Driver: state
// ---------------------------------------------------------------------------

static AxlLoop           *m_loop;
static AxlSshServer      *m_srv;
static AxlConsoleDevice  *m_dev;
static SshdCon            m_con;         ///< what is held of the take-over
static AxlConsoleVtEnc   *m_enc;
static const AxlConsoleOps *m_inner;     ///< the encoder's ops
static void              *m_inner_user;
static AxlConsoleOps      m_wrap;        ///< the counted ops the device calls
static AxlSourceId        m_tick;
static SshdSessions       m_sess;
static SshdState          m_state;
static bool               m_ro;
static bool               m_shell_seen;
static bool               m_muted;       ///< the Shell has gone: no more console output out
static uint64_t           m_drain_until;
static uint32_t           m_banner_cols;
static uint32_t           m_banner_rows;
/** The Banner file's text, read once at setup; ahead of the size line. */
static char               m_banner_user[SSHD_BANNER_FILE_MAX];
static size_t             m_banner_user_len;
/** The banner as sent: the file's text, then the size line. */
static char               m_banner_text[AXL_SSH_BANNER_MAX];
static AxlSerial         *m_serial;      ///< the log's serial port while serving (AxlLogPort), or NULL
static AxlSerial         *m_geom;        ///< --report-geometry's port: the first, or NULL
static AxlSerial         *m_geom_own;    ///< m_geom when it is not m_serial (closed apart), or NULL
static AxlHandle          m_log_h;       ///< the port log_port_pick chose, or NULL
static size_t             m_log_at;      ///< its ordinal
static SshdLogPick        m_log_pick;    ///< where the log goes while serving
static char               m_kept[SSHD_LOG_KEEP_BYTES]; ///< the log in memory (`sshd log`)
static size_t             m_kept_len;    ///< bytes of whole lines in m_kept
static uint64_t           m_kept_gen;    ///< lines ever appended (SshdLogIface::gen)
static bool               m_kept_on;     ///< kept_handler is registered
static AxlHandle          m_kept_h;      ///< SSHD_LOG_GUID's handle, or NULL
static SshdLogIface       m_kept_iface;  ///< SSHD_LOG_GUID's interface
static SshdHostKeyIface   m_hk_iface;    ///< SSHD_HOSTKEY_GUID's interface
static AxlHandle          m_hk_h;        ///< its handle, or NULL
static bool               m_log_moved;   ///< log_to_operator(true) is in effect
static bool               m_log_before;  ///< console logging as log_to_operator found it
static uint32_t           m_fit_cols;    ///< the console size sessions were last fitted to
static uint32_t           m_fit_rows;
static const char        *m_password;
static size_t             m_password_len;
static SshdRateLimit      m_behind_rl;
static SshdRateLimit      m_inflight_rl;
static SshdRateLimit      m_ctrl_rl;

/** Console ops in flight. Written at the Shell's TPL, read by the tick. */
static volatile unsigned  m_in_op;
/** The encoder flushed from inside an op; those bytes reached nobody. */
static volatile bool      m_overflow;
/** Bumped by every console op: what a repaint's snapshot is judged against. */
static volatile uint64_t  m_op_gen;

// ---------------------------------------------------------------------------
// Counted console ops: each forwards to the encoder's op of the same name
// ---------------------------------------------------------------------------

#define OP_ENTER()  (m_op_gen++, m_in_op++)
#define OP_LEAVE()  (m_in_op--)

static void
w_set_cell_rule(void *u, AxlConsoleCellRule rule)
{
    (void)u;
    OP_ENTER();
    m_inner->set_cell_rule(m_inner_user, rule);
    OP_LEAVE();
}

static void
w_clear_screen(void *u)
{
    (void)u;
    OP_ENTER();
    m_inner->clear_screen(m_inner_user);
    OP_LEAVE();
}

static void
w_set_cursor(void *u, int32_t row, int32_t col)
{
    (void)u;
    OP_ENTER();
    m_inner->set_cursor(m_inner_user, row, col);
    OP_LEAVE();
}

static void
w_output_text(void *u, const char *utf8, size_t len)
{
    (void)u;
    OP_ENTER();
    m_inner->output_text(m_inner_user, utf8, len);
    OP_LEAVE();
}

static void
w_set_pen(void *u, const AxlConsolePen *pen)
{
    (void)u;
    OP_ENTER();
    m_inner->set_pen(m_inner_user, pen);
    OP_LEAVE();
}

static void
w_set_mode(void *u, uint32_t mode)
{
    (void)u;
    OP_ENTER();
    m_inner->set_mode(m_inner_user, mode);
    OP_LEAVE();
}

/* `--report-geometry`: the new size, written straight to the serial port at
   the reshape -- after TerminalDxe's own SetMode output, ahead of the clear
   the Shell follows it with -- which is where a reader of that stream (the
   console-visual oracle) needs it, since a serial terminal is never told.
   NOT a log line: that goes through ConOut, so it was drawn on the GOP, on
   the serial line and in every session, from inside the SetMode it reports.
   An APC string, which a VT terminal discards; the harness removes it. One
   write, its status dropped: a report must never stall the console call
   that carries it, and there is nobody to tell. The write is bounded by the
   port's own transmit timeout (axl_serial_write returns a short count when it
   elapses), and a ~30-byte record fits a UART FIFO, so a full FIFO costs at
   most that timeout once per reshape; the shared port's timeout is not
   changed here, as that would retune the console's own serial line. */
static void
geometry_report(uint32_t cols, uint32_t rows)
{
    char rec[48];
    int  n;

    if (m_geom == NULL) {
        return;
    }
    n = axl_snprintf(rec, sizeof(rec), "\x1b_axl-console-size=%ux%u\x1b\\",
                     (unsigned)cols, (unsigned)rows);
    if (n > 0 && (size_t)n < sizeof(rec)) {
        axl_serial_write(m_geom, rec, (size_t)n, NULL);
    }
}

static void
w_resize(void *u, uint32_t cols, uint32_t rows)
{
    (void)u;
    OP_ENTER();
    m_inner->resize(m_inner_user, cols, rows);
    OP_LEAVE();
    geometry_report(cols, rows);
}

static void
w_erase(void *u, AxlConsoleRect rect, bool selective)
{
    (void)u;
    OP_ENTER();
    m_inner->erase(m_inner_user, rect, selective);
    OP_LEAVE();
}

static void
w_moverect(void *u, AxlConsoleRect dest, AxlConsoleRect src)
{
    (void)u;
    OP_ENTER();
    m_inner->moverect(m_inner_user, dest, src);
    OP_LEAVE();
}

static void
w_bell(void *u)
{
    (void)u;
    OP_ENTER();
    m_inner->bell(m_inner_user);
    OP_LEAVE();
}

static int
w_scrollrect(void *u, AxlConsoleRect rect, int32_t downward, int32_t rightward)
{
    (void)u;
    OP_ENTER();
    int r = m_inner->scrollrect(m_inner_user, rect, downward, rightward);
    OP_LEAVE();
    return r;
}

static int
w_set_term_prop(void *u, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    (void)u;
    OP_ENTER();
    int r = m_inner->set_term_prop(m_inner_user, prop, val);
    OP_LEAVE();
    return r;
}

static void
w_clear_scrollback(void *u)
{
    (void)u;
    OP_ENTER();
    m_inner->clear_scrollback(m_inner_user);
    OP_LEAVE();
}

/* The wrapper offers exactly the ops the encoder implements: a NULL op means
   something to a producer (scrollrect NULL = always decline), so a wrapper
   that turned NULL into a forwarding stub would change the encoder's
   behaviour. */
static void
wrap_ops(const AxlConsoleOps *in)
{
    m_wrap = (AxlConsoleOps){
        .set_cell_rule    = in->set_cell_rule    ? w_set_cell_rule    : NULL,
        .clear_screen     = in->clear_screen     ? w_clear_screen     : NULL,
        .set_cursor       = in->set_cursor       ? w_set_cursor       : NULL,
        .output_text      = in->output_text      ? w_output_text      : NULL,
        .set_pen          = in->set_pen          ? w_set_pen          : NULL,
        .set_mode         = in->set_mode         ? w_set_mode         : NULL,
        .resize           = in->resize           ? w_resize           : NULL,
        .erase            = in->erase            ? w_erase            : NULL,
        .moverect         = in->moverect         ? w_moverect         : NULL,
        .bell             = in->bell             ? w_bell             : NULL,
        .scrollrect       = in->scrollrect       ? w_scrollrect       : NULL,
        .set_term_prop    = in->set_term_prop    ? w_set_term_prop    : NULL,
        .clear_scrollback = in->clear_scrollback ? w_clear_scrollback : NULL,
    };
}

// ---------------------------------------------------------------------------
// Output: the encoder's sink, the fan-out and the repaint
// ---------------------------------------------------------------------------

/* An AxlSsh write status, in the binding's words. */
static SshdWrResult
wr_result(int rc)
{
    switch (rc) {
    case AXL_OK:
        return SSHD_WR_OK;
    case AXL_BUSY:
        return SSHD_WR_BUSY;
    case AXL_NOT_FOUND:
        return SSHD_WR_GONE;
    default:
        return SSHD_WR_FAILED;
    }
}

static SshdWrResult
fan_write(const char *bytes, size_t len, void *ud)
{
    return wr_result(axl_ssh_channel_write(m_srv, ((SshdChan *)ud)->ch, bytes, len));
}

/* The session misses live output from here until a repaint catches it up.
   One DEBUG line a second at most, carrying how many it stood for: a client
   that stops reading would otherwise log one per flush. */
static void
go_behind(SshdChan *c, const char *why)
{
    uint32_t more = 0;

    if (sshd_sessions_mark_behind(&m_sess, c)
        && sshd_ratelimit(&m_behind_rl, axl_time_get_ms(), SSHD_BEHIND_LOG_MS, &more)) {
        axl_debug("a session fell behind (%s); it will be repainted (%u more since the "
                  "last report)", why, (unsigned)more);
    }
}

/* A write refused for any reason but BUSY. The session is never written to
   again; a channel still open is closed, or its client would sit connected to
   a session nothing reaches. Its close hook does the rest. */
static void
refused_for_good(SshdChan *c, SshdWrResult r)
{
    c->dead = true;
    if (sshd_bind_close_on(r)) {
        axl_ssh_channel_close(m_srv, c->ch);
    }
}

static void
fan_one(SshdSession *s, void *ud)
{
    const FanOut *f = (const FanOut *)ud;
    SshdChan     *c = (SshdChan *)s->chan;

    switch (sshd_fan_write(f->bytes, f->len, SSHD_CHAN_LIMIT, fan_write, c)) {
    case SSHD_FAN_SENT:
        return;
    case SSHD_FAN_BEHIND:
        c->blocked = true;
        go_behind(c, "its client is not reading");
        return;
    case SSHD_FAN_TOO_BIG:
        go_behind(c, "a burst larger than a channel holds");
        return;
    case SSHD_FAN_FAILED:
        go_behind(c, "a write failed");
        refused_for_good(c, SSHD_WR_FAILED);
        return;
    case SSHD_FAN_GONE:
        go_behind(c, "its channel closed");
        c->dead = true;   /* its close hook is coming */
        return;
    }
}

/* The live stream. Reached from the tick's flush -- or, when the encoder's
   buffer overflows, from inside a console op, where no channel may be
   touched: then the bytes are dropped and every session is repainted. */
static void
enc_sink(const char *bytes, size_t len, void *user)
{
    (void)user;
    if (m_muted) {
        return;   /* the Shell has gone: the firmware's output is not the sessions' */
    }
    if (m_in_op != 0) {
        m_overflow = true;
        return;
    }
    FanOut f = { bytes, len };
    sshd_sessions_each(&m_sess, SSHD_SESS_LIVE, fan_one, &f);
}

static void
snap_sink(const char *bytes, size_t len, void *user)
{
    axl_string_append_len((AxlString *)user, bytes, len);
}

static void
mark_live_behind(SshdSession *s, void *ud)
{
    (void)ud;
    go_behind((SshdChan *)s->chan, "console output overflowed the encoder");
}

/* sshd_repaint_drain()'s writer: one piece onto this session's channel. */
static SshdWrResult
rp_write(const char *bytes, size_t len, void *ud)
{
    SshdChan *c = (SshdChan *)ud;

    c->last_wr = wr_result(axl_ssh_channel_write(m_srv, c->ch, bytes, len));
    return c->last_wr;
}

/* A BEHIND session's repaint: the screen model as one self-contained burst.
   Taken only between ops (the caller checks), and only right after the
   tick's flush, so nothing buffered is in the model but not yet on the wire
   to the live sessions -- the joiner sees each op exactly once.

   A repaint the channel refuses part-way is RESUMED on the next writable
   rather than started over (one larger than the channel's queue could
   otherwise never finish), and judged once it is out: if any console op ran
   since its snapshot, the screen it paints is gone and the next tick takes a
   fresh one. sshd_repaint_drain() is that rule, unit-tested. */
static void
repaint_one(SshdSession *s, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)s->chan;

    if (c->blocked || c->dead) {
        return;
    }
    if (c->rp == NULL && c->fit == SSHD_FIT_SMALLER) {
        write_notice(c);   /* held: no console output until it grows */
        return;
    }
    if (c->rp == NULL) {
        AxlString *snap = axl_string_new(NULL);
        if (snap == NULL) {
            return;
        }
        char   pre[16];
        size_t pn = sshd_fit_prefix(pre, sizeof(pre), c->fit, m_fit_rows, c->region);
        if (axl_string_append_len(snap, pre, pn) != AXL_OK
            || axl_console_vt_enc_snapshot(m_enc, snap_sink, snap) != AXL_OK) {
            axl_string_free(snap);
            return;
        }
        size_t len = axl_string_len(snap);
        c->rp = axl_string_steal(snap);
        axl_string_free(snap);
        if (c->rp == NULL) {
            return;
        }
        sshd_repaint_begin(&c->rps, len, m_op_gen);
        c->region = c->fit == SSHD_FIT_LARGER;
        c->retake = false;   /* this one is at the current fit */
    }
    switch (sshd_repaint_drain(&c->rps, c->rp, m_op_gen, SSHD_PIECE, rp_write, c)) {
    case SSHD_RP_BLOCKED:
        c->blocked = true;
        return;
    case SSHD_RP_REJOIN:
        if (c->retake) {
            c->retake = false;   /* sized for a terminal that has changed since */
            break;
        }
        sshd_sessions_clear_behind(&m_sess, c);
        break;
    case SSHD_RP_RETAKE:
        break;
    case SSHD_RP_FAILED:
        refused_for_good(c, c->last_wr);
        break;
    }
    axl_free(c->rp);
    c->rp = NULL;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/* One Ctrl chord; whether the ring took it. A full ring refuses the key but
   has still fired its notifies (the press is what the Shell's break listens
   for). */
static bool
inject_ctrl(char letter)
{
    uint32_t more = 0;

    if (axl_console_device_inject_key_ex(m_dev, 0, (uint16_t)(uint8_t)letter,
                                         SSHD_CTRL_SHIFT, AXL_CONSOLE_TOGGLE_KEYBOARD)
        != AXL_BUSY) {
        return true;
    }
    if (sshd_ctrl_bypasses(letter)
        && sshd_ratelimit(&m_ctrl_rl, axl_time_get_ms(), SSHD_BEHIND_LOG_MS, &more)) {
        axl_debug("Ctrl-%c met a full key ring: its notify fired, the key itself is "
                  "dropped (%u more since the last report)", letter, (unsigned)more);
    }
    return false;
}

/* Give the console the front of one run; how many bytes it took. Text goes in
   whole characters and sequences while the key ring has room. A control key
   is all or nothing: a refused one waits for room like text (sshd_input_route
   never queues a Ctrl-C, so only ordered keys are offered again), and an
   ordered one also waits until the guest has read every key ahead of it, so
   its notify acts after them (sshd_ctrl_may_enter: the Shell's Ctrl-S). Each offer
   fires the key's notifies, so a refused ordered key's fire again on the
   retry; the only other chord the Shell registers, Ctrl-S, sets a flag, so a
   repeat changes nothing. A refusal
   that waiting cannot fix (the device has gone) takes everything, so a dead
   queue cannot wedge the ones behind it. */
static size_t
input_take(SshdInKind kind, const char *bytes, size_t len, void *ud)
{
    (void)ud;
    if (m_dev == NULL) {
        return len;
    }
    if (kind == SSHD_IN_CTRL) {
        /* The ring is shared: another session's continuous paste keeps it
           non-empty and holds this session's ^S until that paste ends
           (bounded by the paste). */
        if (!sshd_ctrl_may_enter(bytes[0], axl_console_device_queued_keys(m_dev))) {
            return 0;   /* waits for the guest to read what is ahead of it */
        }
        return inject_ctrl(bytes[0]) ? len : 0;
    }
    size_t used = 0;
    if (axl_console_device_inject_text_partial(m_dev, bytes, len, &used) == AXL_ERR) {
        return len;
    }
    return used;
}

/* One reassembled run from a session (sshd_input_route): input goes straight
   to the console unless that session already has input waiting, which it
   must not overtake; what the console cannot take yet waits in the session's
   queue -- control keys included, as a pty keeps them in line.

   Ctrl-C alone never waits: it goes in now, ahead of any pending text, as a
   pty's interrupt character acts at once. Its job is the Shell's break
   notify, which fires on the press even when the key ring is full and the
   key itself is dropped -- as a keyboard does. So Ctrl-C breaks a script
   behind a long paste the script never reads, and a refused one is not
   retried (that would fire its notify twice). */
static void
on_input(SshdInKind kind, const char *bytes, size_t len, void *ud)
{
    SshdChan *c = (SshdChan *)ud;

    /* Once the Shell has gone, keys would reach whatever reads the console
       next (the boot manager), which is not the session's to drive. */
    if (m_dev == NULL || c == NULL || m_muted) {
        return;
    }
    if (!sshd_input_route(&c->pend, kind, bytes, len, input_take, NULL)) {
        axl_debug("input dropped (%zu bytes): the session's pending input is full "
                  "(%llu bytes dropped in all)", len, (unsigned long long)c->pend.dropped);
    }
}

/* Per session, each tick: what waited for room first, then whatever the
   reassembler resolves on its timeout (which queues behind it). */
static void
tick_input(SshdSession *s, void *ud)
{
    SshdChan *c = (SshdChan *)s->chan;

    if (m_dev != NULL) {
        sshd_pend_drain(&c->pend, input_take, NULL);
    }
    sshd_input_tick(&s->in, *(const uint64_t *)ud, on_input, c);
}

// ---------------------------------------------------------------------------
// Session channel hooks
// ---------------------------------------------------------------------------

static void
chan_free(SshdChan *c)
{
    if (c != NULL) {
        sftp_relay_close(c->sftp);
        sshd_pend_free(&c->pend);
        axl_free(c->rp);
        axl_free(c);
    }
}

static int
chan_on_open(AxlSshChannel ch, void *principal, const char *user, void *ud)
{
    (void)principal;
    (void)user;
    (void)ud;
    SshdChan *c = axl_calloc(1, sizeof(*c));

    if (c == NULL) {
        return AXL_NO_RESOURCES;
    }
    c->ch = ch;
    if (axl_ssh_channel_set_user_data(m_srv, ch, c) != AXL_OK) {
        axl_free(c);
        return AXL_ERR;
    }
    return AXL_OK;
}

/* Accepted so `ssh -t` gets the terminal it asks for. Its size decides how the
   console is shown on it (see "size" at the top); the console keeps its own. */
static int
chan_on_pty(AxlSshChannel ch, const AxlSshPty *pty, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    if (c != NULL) {
        c->term_cols = pty->cols;
        c->term_rows = pty->rows;
    }
    return AXL_OK;
}

/* The terminal was resized: show the console to it at the new size. */
static void
chan_on_resize(AxlSshChannel ch, const AxlSshPty *size, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    if (c == NULL) {
        return;
    }
    c->term_cols = size->cols;
    c->term_rows = size->rows;
    if (sshd_sessions_find(&m_sess, c) != NULL) {
        chan_refit(c);
    }
}

static int
chan_on_shell(AxlSshChannel ch, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    if (c == NULL || m_state != SSHD_RUNNING) {
        return AXL_ERR;
    }
    if (sshd_sessions_find(&m_sess, c) != NULL) {
        axl_debug("a second shell request on a channel already serving one; refused");
        return AXL_ERR;
    }
    if (sshd_sessions_add(&m_sess, c) == NULL) {
        axl_debug("session table full (%u); shell refused", (unsigned)SSHD_SESSIONS_MAX);
        return AXL_ERR;
    }
    /* BEHIND from birth: the tick repaints it as its first output (or, for a
       terminal smaller than the console, writes the notice). */
    chan_refit(c);
    return AXL_OK;
}

/* No private shell exists to run a command in: the console is shared and
   live. ssh(1) reports the refusal itself. */
static int
chan_on_exec(AxlSshChannel ch, const char *cmd, size_t len, void *ud)
{
    (void)ch;
    (void)cmd;
    (void)len;
    (void)ud;
    return AXL_ERR;
}

/* `sftp`, when sshd.conf has `Subsystem sftp`: the relay opens a session on
   the helper it loaded as sshd started, and the channel is an SFTP session
   from here on. */
static int
chan_on_subsystem(AxlSshChannel ch, const char *name, size_t len, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    if (c == NULL || m_state != SSHD_RUNNING) {
        return AXL_ERR;
    }
    return sftp_relay_open(ch, name, len, &c->sftp);
}

static void
chan_on_eof(AxlSshChannel ch, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    if (c != NULL) {
        sftp_relay_eof(c->sftp);
    }
}

static void
chan_on_data(AxlSshChannel ch, const uint8_t *buf, size_t len, void *ud)
{
    (void)ud;
    SshdChan    *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    if (c != NULL && c->sftp != NULL) {
        sftp_relay_data(c->sftp, buf, len);
        return;
    }
    SshdSession *s = c != NULL ? sshd_sessions_find(&m_sess, c) : NULL;

    if (s == NULL || m_ro || m_dev == NULL) {
        return;
    }
    sshd_input_feed(&s->in, buf, len, axl_time_get_ms(), on_input, c);
}

static void
chan_on_writable(AxlSshChannel ch, void *ud)
{
    (void)ud;
    SshdChan *c = (SshdChan *)axl_ssh_channel_user_data(m_srv, ch);

    /* The repaint itself waits for the tick, which knows whether an op is in
       flight. */
    if (c != NULL) {
        c->blocked = false;
        sftp_relay_writable(c->sftp);
    }
}

static void
chan_on_close(AxlSshChannel ch, void *chan_data, void *ud)
{
    (void)ch;
    (void)ud;
    SshdChan *c = (SshdChan *)chan_data;

    if (c != NULL) {
        sshd_sessions_remove(&m_sess, c);
        chan_free(c);
    }
}

/* Fit this session's terminal to the console as it is now. Owed a repaint (or
   the notice) either way: the region, or the whole screen, has to change. A
   repaint already part-way out finishes first -- cutting it would cut an
   escape sequence -- and is then taken again at the new fit. */
static void
chan_refit(SshdChan *c)
{
    SshdFit was = c->fit;

    c->fit    = sshd_fit(m_fit_cols, m_fit_rows, c->term_cols, c->term_rows);
    c->notice = c->fit == SSHD_FIT_SMALLER;
    if (c->rp != NULL) {
        c->retake = true;
    }
    if (sshd_sessions_mark_behind(&m_sess, c) || was != c->fit) {
        axl_debug("a %ux%u terminal %s the %ux%u console",
                  (unsigned)c->term_cols, (unsigned)c->term_rows,
                  c->fit == SSHD_FIT_SMALLER   ? "is held: smaller than"
                  : c->fit == SSHD_FIT_LARGER ? "is confined to"
                                              : "matches",
                  (unsigned)m_fit_cols, (unsigned)m_fit_rows);
    }
}

static void
refit_one(SshdSession *s, void *ud)
{
    (void)ud;
    chan_refit((SshdChan *)s->chan);
}

/* The console changed size (a `mode` in the Shell): every session's fit may
   have changed with it. */
static void
fit_sync(void)
{
    uint32_t cols = 0;
    uint32_t rows = 0;

    axl_console_device_get_size(m_dev, &cols, &rows);
    if (cols == m_fit_cols && rows == m_fit_rows) {
        return;
    }
    m_fit_cols = cols;
    m_fit_rows = rows;
    sshd_sessions_each(&m_sess, SSHD_SESS_ANY, refit_one, NULL);
}

/* A held session's screen: the notice, once per refit. One write; a channel
   that cannot take it now gets it once it is writable. */
static void
write_notice(SshdChan *c)
{
    char text[256];

    if (!c->notice) {
        return;
    }
    size_t       n = sshd_held_notice(text, sizeof(text), m_fit_cols, m_fit_rows,
                                      c->term_cols, c->term_rows);
    SshdWrResult r = wr_result(axl_ssh_channel_write(m_srv, c->ch, text, n));
    if (r == SSHD_WR_OK) {
        c->notice = false;
        c->region = false;   /* the notice resets it */
    } else if (r == SSHD_WR_BUSY) {
        c->blocked = true;
    } else {
        refused_for_good(c, r);
    }
}

// ---------------------------------------------------------------------------
// Authentication: the optional password
// ---------------------------------------------------------------------------

static void
on_password(AxlSshAuthRequest req, const char *user, const char *password,
            size_t password_len, void *ud)
{
    (void)user;
    (void)ud;
    /* Length first, then the bytes in constant time against the CONFIGURED
       length, so the time taken says nothing about how much matched. */
    bool ok = password_len == m_password_len
              && axl_consttime_equal(password, m_password, m_password_len);

    if (axl_ssh_auth_complete(m_srv, req, ok ? AXL_SSH_AUTH_ACCEPT : AXL_SSH_AUTH_DENY,
                              NULL, NULL) != AXL_OK) {
        axl_debug("password verdict not delivered");
    }
}

// ---------------------------------------------------------------------------
// The console's size, as the pre-authentication banner
// ---------------------------------------------------------------------------

/* Every session shows the one console, so the terminal has to be at least its
   size (a smaller one is held behind a notice); the banner says so before the
   session starts, where ssh(1) prints it -- not inside the session, where the
   repaint's clear would wipe it at once.
   Re-sent whenever the size changes (a `mode` in the Shell): the banner is
   read per connection, so the next login sees the new size. */
static void
banner_sync(void)
{
    uint32_t cols = 0;
    uint32_t rows = 0;

    axl_console_device_get_size(m_dev, &cols, &rows);
    if (cols == m_banner_cols && rows == m_banner_rows) {
        return;
    }
    size_t n = sshd_banner_compose(m_banner_text, sizeof(m_banner_text), m_banner_user,
                                   m_banner_user_len, cols, rows);
    if (n > 0 && axl_ssh_server_set_banner(m_srv, m_banner_text) == AXL_OK) {
        m_banner_cols = cols;
        m_banner_rows = rows;
    }
}

// ---------------------------------------------------------------------------
// The tick, and the Shell exiting
// ---------------------------------------------------------------------------

static void
say_goodbye(SshdSession *s, void *ud)
{
    SshdChan *c = (SshdChan *)s->chan;
    char      text[160];
    size_t    n;

    /* A scroll region set for a larger terminal goes with the session: the
       user's own shell gets the whole terminal back. */
    n = sshd_bind_goodbye_text(text, sizeof(text), c->region, (const char *)ud);
    if (!c->dead && n > 0
        && axl_ssh_channel_write(m_srv, c->ch, text, n) != AXL_OK) {
        axl_debug("goodbye not queued on a stalled session");
    }
    if (axl_ssh_channel_exit(m_srv, c->ch, 0) != AXL_OK) {
        axl_ssh_channel_close(m_srv, c->ch);
    }
}

/* The Shell this driver was loaded into has gone (`exit`): there is no
   console left to serve. Give the take-over back first, so whatever the
   firmware runs next owns its console, then tell the sessions and let them
   drain before the server goes. */
static bool
release_console(void)
{
    /* The Shell's exit path and the firmware after it still print, and this
       tick can preempt one of those calls part-way through the encoder. The
       device refuses while a call into it is live; try again next tick. */
    SshdRelease r = sshd_bind_release_of(console_drop());

    if (r == SSHD_REL_RETRY) {
        return false;
    }
    if (r == SSHD_REL_RELEASED) {
        axl_info("the UEFI Shell exited; console released");
    } else {
        /* console_drop() has said why; the take-over stays in place. The
           driver stays up with nothing to serve, so it is degraded until a
           stop's teardown releases the console: status says so. */
        axl_error("the UEFI Shell exited; the console could not be released");
        axl_service_set_degraded(true, SSHD_STUCK_REASON);
    }
    sshd_sessions_each(&m_sess, SSHD_SESS_ANY, say_goodbye,
                       (void *)sshd_bind_goodbye(r));
    m_state       = SSHD_DRAINING;
    m_drain_until = axl_time_get_ms() + SSHD_DRAIN_MS;
    return true;
}

static void
server_drop(void)
{
    axl_ssh_server_free(m_srv);   /* runs every remaining close hook */
    m_srv = NULL;
    sftp_relay_shutdown();        /* after them: no session is left */
}

static bool
on_tick(void *data)
{
    (void)data;
    uint64_t now = axl_time_get_ms();

    if (m_state == SSHD_DRAINING) {
        if (sshd_sessions_count(&m_sess) == 0 || now >= m_drain_until) {
            server_drop();
            m_state = SSHD_DONE;
            m_tick  = 0;
            return AXL_SOURCE_REMOVE;
        }
        return AXL_SOURCE_CONTINUE;
    }

    /* Bytes the encoder flushed inside an op reached no one: everybody is
       owed a repaint, and what is buffered now continues a stream nobody
       has. */
    if (m_overflow) {
        m_overflow = false;
        sshd_sessions_each(&m_sess, SSHD_SESS_LIVE, mark_live_behind, NULL);
    }
    /* The Shell is looked for FIRST: once it has gone, what the console
       shows is the firmware's (the boot manager starting the next option),
       and none of it is the session's -- the goodbye is the last thing a
       session is sent. So output stops here, before this tick's flush. */
    bool shell_up = axl_shell_kind() == AXL_SHELL_KIND_UEFI;
    if (shell_up) {
        m_shell_seen = true;
    } else if (m_shell_seen) {
        m_muted = true;
    }
    fit_sync();
    bool between_ops = sshd_bind_snapshot_allowed(m_in_op);
    if (between_ops && !m_muted) {
        axl_console_vt_enc_flush(m_enc);
    }
    /* Muted, a session's keys are dropped (on_input), queued ones included.
       The mute also drops at most one tick of the Shell's own last output:
       what it printed after this tick's flush and before it was seen gone. */
    if (!m_muted) {
        sshd_sessions_each(&m_sess, SSHD_SESS_ANY, tick_input, &now);
    }
    if (between_ops && !m_muted) {
        sshd_sessions_each(&m_sess, SSHD_SESS_BEHIND, repaint_one, NULL);
    }
    banner_sync();

    if (m_muted && sshd_bind_snapshot_allowed(m_in_op)) {
        /* Not while one of our ops is part-way: the encoder it is running in
           is freed by the release. */
        uint32_t more = 0;
        if (!release_console()
            && sshd_ratelimit(&m_inflight_rl, now, SSHD_BEHIND_LOG_MS, &more)) {
            axl_debug("the UEFI Shell exited; a console call is in flight, "
                      "releasing on a later tick (%u more since the last report)",
                      (unsigned)more);
        }
    }
    return AXL_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Service lifecycle
// ---------------------------------------------------------------------------

/* The Banner file's text into m_banner_user, once: the launcher checked it
   is there, text and small enough, so a failure here is a file that changed
   in between. */
static int
banner_read(const char *path)
{
    void          *buf  = NULL;
    size_t         len  = 0;
    char          *text = NULL;
    size_t         n    = 0;
    SshdBannerRead r;

    m_banner_user_len = 0;
    if (path == NULL || path[0] == '\0') {
        return AXL_OK;
    }
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        axl_error("cannot read the Banner file %s", path);
        return AXL_ERR;
    }
    r = sshd_bind_banner_text(buf, len, sizeof(m_banner_user), &text, &n);
    axl_free(buf);
    if (r == SSHD_BANNER_NO_MEMORY) {
        axl_error("out of memory reading the Banner file %s", path);
        return AXL_ERR;
    }
    if (r != SSHD_BANNER_OK) {
        axl_error("the Banner file %s is %s", path,
                  r == SSHD_BANNER_TOO_LARGE ? "too large" : "not UTF-8 text");
        return AXL_ERR;
    }
    axl_memcpy(m_banner_user, text, n);
    m_banner_user_len = n;
    axl_free(text);
    return AXL_OK;
}

/* A setter's status, logged against its sshd.conf keyword when it refused. */
static bool
refused(const char *keyword, int rc)
{
    if (rc != AXL_OK) {
        axl_error("%s refused by the server", keyword);
        return true;
    }
    return false;
}

/* Every sshd.conf server setting through its public setter, before the
   server listens, in order and stopping at the first refusal. A refusal
   names the keyword: the launcher validated the value against the same
   bounds, so it means the two disagree. */
static int
server_settings(const SshdOpts *o)
{
    AxlSshPenalties pen;
    int             prc = AXL_OK;

    if (o->penalties) {
        prc = axl_ssh_penalties_default(&pen);
        pen.authfail_sec       = o->pen_authfail;
        pen.noauth_sec         = o->pen_noauth;
        pen.grace_exceeded_sec = o->pen_grace;
        pen.min_sec            = o->pen_min;
        pen.max_sec            = o->pen_max;
    }
    if (refused("LoginGraceTime", axl_ssh_server_set_login_grace(m_srv, o->grace_sec))
        || refused("MaxAuthTries",
                   axl_ssh_server_set_max_password_failures(m_srv, o->max_auth))
        || refused("MaxSessions", axl_ssh_server_set_max_sessions(m_srv, o->max_sessions))
        || refused("MaxStartups", axl_ssh_server_set_max_startups(m_srv, o->max_startups))
        || refused("ClientAliveInterval",
                   axl_ssh_server_set_client_alive(m_srv, o->alive_sec, o->alive_max))
        || refused("UnusedConnectionTimeout",
                   axl_ssh_server_set_unused_timeout(m_srv, o->unused_sec))
        || refused("RekeyLimit", axl_ssh_server_set_rekey_limit(m_srv, o->rekey_bytes))
        || refused("PerSourcePenalties", prc)
        || refused("PerSourcePenalties",
                   axl_ssh_server_set_penalties(m_srv, o->penalties ? &pen : NULL))
        || refused("PerSourcePenaltyExemptList", apply_exempt(o->exempt))) {
        return AXL_ERR;
    }
    return AXL_OK;
}

/* Each comma-separated range of `list`, through the public setter. */
static int
apply_exempt(const char *list)
{
    if (list == NULL || list[0] == '\0') {
        return AXL_OK;
    }
    char **f = axl_strsplit(list, ',');
    size_t n = 0;

    if (f == NULL) {
        return AXL_NO_RESOURCES;
    }
    while (f[n] != NULL) {
        n++;
    }
    int rc = axl_ssh_server_set_penalty_exempt(m_srv, (const char *const *)f, n);
    axl_strfreev(f);
    return rc;
}

/* The host key a HostKey list names: the first entry that loads, with
   sshd's posture for the rest (sshd.c:1476-1563) -- each one that cannot is
   reported, with sshd's two lines, and skipped; `none` loads nothing. The
   launcher checked the list already, without the provider; this is the load
   that decides, so a seed that does not derive its public key -- which only
   the provider can see -- is skipped here like any other (review M1). */
static int
host_key_from_files(
    const char  *list,  ///< SshdOpts::host_keys
    AxlPkKey   **key    ///< [out] the key
)
{
    const char *cur = list;
    char        path[SSHD_HOSTKEY_PATH_MAX];

    while (sshd_bind_hostkey_next(&cur, path, sizeof path)) {
        if (axl_strcmp(path, SSHD_HOSTKEY_NONE) == 0) {
            continue;
        }
        AxlSshHostKeyRecord rec;
        const char         *why = NULL;
        int                 rc  = axl_ssh_host_key_read_file(path, &rec, &why);
        if (rc == AXL_OK) {
            rc = axl_ssh_host_key_load(&rec, key, &why);
        }
        axl_ssh_host_key_record_clear(&rec);
        if (rc == AXL_OK) {
            axl_strlcpy(m_hk_iface.path, path, sizeof m_hk_iface.path);
            return AXL_OK;
        }
        if (rc != AXL_NOT_FOUND) {
            axl_error("Unable to load host key \"%s\": %s", path, why);
        }
        axl_error("Unable to load host key: %s", path);
    }
    axl_error("no hostkeys available -- exiting.");
    return AXL_ERR;
}

/* Give the server its host key (P5): a HostKey file, or the one stored in
   the UEFI variable -- made and stored at a first start. A stored key that
   is present but unusable, or unreadable, stops the setup: it is never
   replaced silently. One that could not be stored serves anyway, and marks
   the service DEGRADED so `sshd status` says so. The interface `sshd`
   reads is filled here and published once the server serves. */
static int
host_key_install(
    const SshdOpts *o  ///< the options
)
{
    AxlPkKey   *key = NULL;
    const char *why = NULL;

    axl_memset(&m_hk_iface, 0, sizeof m_hk_iface);
    m_hk_iface.size    = (uint32_t)sizeof m_hk_iface;
    m_hk_iface.version = SSHD_HOSTKEY_IFACE_VERSION;
    if (o->host_keys != NULL && o->host_keys[0] != '\0') {
        if (host_key_from_files(o->host_keys, &key) != AXL_OK) {
            return AXL_ERR;
        }
        m_hk_iface.origin = SSHD_HOSTKEY_FILE;
    } else {
        AxlSshHostKeyOrigin origin = AXL_SSH_HOST_KEY_LOADED;
        int                 rc     = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
        if (rc == AXL_IO_ERROR) {
            axl_error("the stored host key cannot be read (%s); not serving -- run"
                      " 'sshd hostkey --regenerate'", why != NULL ? why : "?");
            return AXL_ERR;
        }
        if (rc == AXL_ERR) {
            axl_error("the stored host key is unusable (%s); not serving -- run"
                      " 'sshd hostkey --regenerate'", why != NULL ? why : "?");
            return AXL_ERR;
        }
        if (rc != AXL_OK) {
            axl_error("cannot make a host key (%s)", why != NULL ? why : "out of memory");
            return AXL_ERR;
        }
        m_hk_iface.origin = origin == AXL_SSH_HOST_KEY_GENERATED ? SSHD_HOSTKEY_GENERATED
                            : origin == AXL_SSH_HOST_KEY_EPHEMERAL ? SSHD_HOSTKEY_EPHEMERAL
                                                                   : SSHD_HOSTKEY_LOADED;
    }
    if (axl_ssh_server_set_host_key(m_srv, key) != AXL_OK) {
        axl_pk_key_free(key);
        axl_error("cannot install the host key");
        return AXL_ERR;
    }
    if (axl_ssh_server_host_key_fingerprint(m_srv, m_hk_iface.fingerprint,
                                            sizeof m_hk_iface.fingerprint) != AXL_OK
        || axl_ssh_server_host_key_public_line(m_srv, m_hk_iface.public_line,
                                               sizeof m_hk_iface.public_line) != AXL_OK) {
        axl_error("cannot fingerprint the host key");
        return AXL_ERR;
    }
    char where[SSHD_HOSTKEY_PATH_MAX + 16];
    if (m_hk_iface.origin == SSHD_HOSTKEY_FILE) {
        axl_snprintf(where, sizeof where, "file %s", m_hk_iface.path);
    } else {
        axl_snprintf(where, sizeof where, "UEFI variable %s", AXL_SSH_HOST_KEY_VAR);
    }
    switch (m_hk_iface.origin) {
    case SSHD_HOSTKEY_GENERATED:
        axl_info("generated a new host key: ssh-ed25519 %s (%s)",
                 m_hk_iface.fingerprint, where);
        break;
    case SSHD_HOSTKEY_EPHEMERAL:
        axl_strlcpy(m_hk_iface.why, why != NULL ? why : "", sizeof m_hk_iface.why);
        axl_error("cannot save the host key in UEFI variable %s (%s): serving with a"
                  " key that changes at every start", AXL_SSH_HOST_KEY_VAR,
                  m_hk_iface.why);
        if (axl_service_set_degraded(true, SSHD_HOSTKEY_DEGRADED_REASON) != AXL_OK) {
            axl_error("cannot mark the service degraded");
        }
        break;
    default:
        break;
    }
    axl_info("host key ssh-ed25519 %s (%s)", m_hk_iface.fingerprint, where);
    return AXL_OK;
}

static int
server_build(SshdOpts *o)
{
    static const AxlSshChannelOps chan_ops = {
        .open      = chan_on_open,
        .pty       = chan_on_pty,
        .shell     = chan_on_shell,
        .exec      = chan_on_exec,
        .subsystem = chan_on_subsystem,
        .data      = chan_on_data,
        .eof       = chan_on_eof,
        .resize    = chan_on_resize,
        .writable  = chan_on_writable,
        .close     = chan_on_close,
    };
    static const AxlSshAuthOps auth_ops = {
        .password = on_password,
    };

    /* AxlTcpLinger. Pacing is process-wide in AxlTcp, and this driver image
       carries its own AxlTcp, so the setting is sshd's alone. The launcher
       range-checked it; a value past AxlTcp's range here is a bug. */
    AxlTcpPacing pacing;
    axl_tcp_get_pacing(&pacing);
    pacing.linger_us = o->tcp_linger_us;
    if (axl_tcp_set_pacing(&pacing) != AXL_OK) {
        axl_error("AxlTcpLinger %u us is out of range (0-%u)", (unsigned)o->tcp_linger_us,
                  (unsigned)AXL_TCP_PACING_LINGER_MAX_US);
        return AXL_ERR;
    }

    m_srv = axl_ssh_server_new(m_loop, o->net.port);
    if (m_srv == NULL) {
        axl_error("cannot create the SSH server");
        return AXL_ERR;
    }
    if (host_key_install(o) != AXL_OK) {
        return AXL_ERR;
    }
    /* PubkeyAuthentication no: no keys, so the server offers no publickey
       login; PasswordAuthentication no: no hook, so it offers no password
       one. */
    if (o->pubkey && o->keys != NULL && o->keys[0] != '\0'
        && axl_ssh_server_load_authorized_keys(m_srv, o->keys) != AXL_OK) {
        axl_error("cannot load %s (ssh-ed25519 lines only, at most 16)", o->keys);
        return AXL_ERR;
    }
    if (o->pwauth && m_password_len > 0
        && axl_ssh_server_set_auth_ops(m_srv, &auth_ops, NULL) != AXL_OK) {
        axl_error("cannot install the password check");
        return AXL_ERR;
    }
    if (server_settings(o) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_ssh_server_set_channel_ops(m_srv, &chan_ops, NULL) != AXL_OK
        || axl_ssh_server_set_channel_write_limit(
               m_srv, SSHD_CHAN_LIMIT, AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT) != AXL_OK) {
        axl_error("cannot configure session channels");
        return AXL_ERR;
    }
    int level = sshd_bind_log_level(o->loglevel);
    SftpRelayConfig sftp = {
        .path      = o->sftp_path != NULL ? o->sftp_path : "",
        .internal  = sftp_command_is_internal(o->sftp),
        .enabled   = o->sftp != NULL && o->sftp[0] != '\0',
        .conf      = o->conf != NULL ? o->conf : "",
        /* A session that cannot type cannot write files either. */
        .read_only = o->sftp_ro || o->ro,
        .read_only_forced = o->ro && !o->sftp_ro,
        .log_level = level >= 0 ? level : AXL_LOG_INFO,
        /* Half the channel limit, so a refused piece fits again as soon as
           the queue is below half (the writable hook's condition). */
        .write_max = SSHD_CHAN_LIMIT / 2u,
        .loop      = m_loop,
    };
    if (sftp_relay_init(m_srv, &sftp) != AXL_OK) {
        axl_error("cannot configure the sftp subsystem");
        return AXL_ERR;
    }
    return AXL_OK;
}

static int
console_take(void)
{
    AxlConsoleVtEncConfig ecfg = {
        .sink     = enc_sink,
        .coalesce = true,
    };
    AxlConsoleDeviceConfig dcfg = {
        /* Co-paint: the local console keeps working and showing everything. */
        .passthrough_local = true,
        /* Take input so sessions can type; read_physical keeps the local
           keyboard (and serial) typing too. Both or neither. */
        .take_input        = true,
        .read_physical     = true,
        .auto_alt_screen   = true,
        .take_pointer      = false,
    };
    uint32_t cols = 0;
    uint32_t rows = 0;

    m_enc = axl_console_vt_enc_new(&ecfg);
    if (m_enc == NULL) {
        return AXL_NO_RESOURCES;
    }
    m_inner = axl_console_vt_enc_ops(m_enc, &m_inner_user);
    wrap_ops(m_inner);
    if (axl_console_device_install(&m_wrap, NULL, &dcfg, &m_dev) != AXL_OK) {
        m_dev = NULL;
        axl_console_vt_enc_free(m_enc);
        m_enc = NULL;
        return AXL_ERR;
    }
    m_con = SSHD_CON_TAKEN;
    /* Passthrough takes the physical console's size, which is whatever the
       firmware chose; the screen model must repaint at exactly that. */
    axl_console_device_get_size(m_dev, &cols, &rows);
    axl_console_vt_enc_set_size(m_enc, cols, rows);
    return AXL_OK;
}

static int
console_drop(void)
{
    if (m_con == SSHD_CON_NONE) {
        return AXL_OK;
    }
    /* Device first: it reports ops into the encoder until it is gone. AXL_BUSY
       (a call into the device is in flight -- the tick preempted the console)
       changes nothing, and the caller retries. A device the firmware would not
       let go of still calls into the encoder and into this image, so both the
       handle and the encoder are KEPT: the next attempt (`sshd stop`) resumes
       on the same handle, and until one succeeds the image refuses to unload. */
    int rc = axl_console_device_uninstall(m_dev);
    SshdRelease r = sshd_bind_con_drop(&m_con, rc);

    if (r == SSHD_REL_RETRY) {
        return rc;
    }
    if (r == SSHD_REL_STUCK) {
        /* What the service state says about it is the caller's: degraded
           while running, stopping from a setup or a teardown. */
        axl_error("the console take-over could not be undone (%d); it stays in "
                  "place until 'sshd stop' succeeds", rc);
        return rc;
    }
    m_dev = NULL;
    axl_console_vt_enc_free(m_enc);
    m_enc = NULL;
    return AXL_OK;
}

/* Where the log goes while serving (AxlLogPort), decided before anything
   with side effects, so a port the machine lacks refuses the start. auto
   never takes a port the console draws on: the serial terminal driver draws
   by absolute cursor position from its own cursor model, so a line it did
   not write scrolls the operator's screen under it, and the Shell's typed
   line and output are painted from then on over sshd's lines -- with -v,
   one per packet, which buries them. OpenSSH's sshd never writes the
   console either: its log is syslog's. */
static int
log_port_pick(uint64_t want)
{
    AxlHandle ports[SSHD_LOG_PORTS_MAX];
    bool      console[SSHD_LOG_PORTS_MAX];
    size_t    n  = 0;
    size_t    at = 0;

    for (AxlHandle h = axl_serial_next(NULL); h != NULL && n < SSHD_LOG_PORTS_MAX;
         h = axl_serial_next(h)) {
        ports[n]   = h;
        console[n] = axl_serial_is_console(h);
        n++;
    }
    m_log_h    = NULL;
    m_log_pick = sshd_bind_log_port_pick(want, console, n, &at);
    if (m_log_pick == SSHD_LOG_NO_SUCH_PORT) {
        axl_error("AxlLogPort/--log-port %llu: no such serial port (%zu here)",
                  (unsigned long long)want, n);
        return AXL_ERR;
    }
    if (m_log_pick == SSHD_LOG_TO_PORT) {
        m_log_h  = ports[at];
        m_log_at = at;
    }
    return AXL_OK;
}

/* Open what log_port_pick chose, and --report-geometry's port: the first,
   which is the harness's console stream whatever the log's port is (its
   record is an APC string a terminal discards, not a line). One open when
   they are the same port: an open is exclusive. Said at DEBUG while the log
   is still on the console, so `sshd -v` tells the operator where to look. */
static void
log_port_open(bool geometry)
{
    AxlHandle first = axl_serial_next(NULL);

    if (m_log_h != NULL && axl_serial_open(m_log_h, &m_serial) != AXL_OK) {
        m_serial = NULL;
    }
    if (m_serial != NULL) {
        axl_debug("the log goes to serial port %zu while serving", m_log_at);
    } else {
        /* log_port_pick refused NO_SUCH_PORT, so it never gets here. */
        axl_debug("the log is not written while serving: %s",
                  m_log_pick == SSHD_LOG_OFF_NONE ? "AxlLogPort none"
                  : m_log_pick == SSHD_LOG_OFF_CONSOLE
                      ? "every serial port carries the console"
                      : "open failed");
    }
    if (!geometry || first == NULL) {
        return;
    }
    if (first == m_log_h) {
        m_geom = m_serial;
    } else if (axl_serial_open(first, &m_geom_own) == AXL_OK) {
        m_geom = m_geom_own;
    } else {
        m_geom_own = NULL;
    }
}

/* The log's sink in memory: whole `[LEVEL] domain: message` lines, the
   oldest dropped on a line start to make room (sshd_bind_log_append). A text buffer rather than AxlLogRing: the ring and the ring
   buffer under it cost the image more than this whole sink, and sshd sits
   at its size limit. */
static void
kept_handler(int level, const char *domain, const char *message,
             const AxlRealtime *stamp, void *data)
{
    static const char *const names[] = { "ERROR", "WARNING", "INFO", "DEBUG", "TRACE" };
    char line[SSHD_LOG_WIDTH];

    (void)stamp;
    (void)data;
    int n = axl_snprintf(line, sizeof(line), "[%s] %s: %s\n",
                         names[level >= 0 && level <= AXL_LOG_TRACE ? level : 0],
                         domain != NULL ? domain : "", message);
    if (n <= 0) {
        return;
    }
    if (n >= SSHD_LOG_WIDTH) {
        n = SSHD_LOG_WIDTH - 1;
        line[n - 1] = '\n';
    }
    /* Logs come at the Shell's TPL as well as the tick's (a `mode` reaches
       the console device's ops), so the append is made at TPL_NOTIFY: one
       writer at a time, and the reader copies at the same TPL. */
    unsigned tpl = axl_tpl_raise(AXL_TPL_NOTIFY);
    m_kept_len = sshd_bind_log_append(m_kept, sizeof(m_kept), m_kept_len, line, (size_t)n);
    m_kept_gen++;
    axl_tpl_restore(tpl);
}

/* Keep the log in memory from the moment sshd serves, published for
   `sshd log` and a refused `sshd stop` -- on a machine whose every serial
   port is the console's, it is the only place the log goes. Withdrawn only
   when the image really unloads; the handler is left registered then, as
   the handler table is this image's own and goes with it. */
static void
kept_keep(bool on)
{
    if (on && !m_kept_on) {
        m_kept_on = axl_log_add_handler(kept_handler, NULL) == AXL_OK;
        m_kept_iface.text = m_kept;
        m_kept_iface.len  = &m_kept_len;
        m_kept_iface.gen  = &m_kept_gen;
        if (m_kept_on
            && axl_protocol_install(&SSHD_LOG_GUID, &m_kept_iface, &m_kept_h) != AXL_OK) {
            m_kept_h = NULL;
        }
    } else if (!on && m_kept_h != NULL) {
        axl_protocol_uninstall(m_kept_h, &SSHD_LOG_GUID, &m_kept_iface);
        m_kept_h = NULL;
    }
}

/* sshd's own log is the operator's, never the user's terminal -- OpenSSH's
   goes to syslog. The device joined the StdErr fan-out, so while it serves,
   this image's log goes to the serial port log_port_pick chose instead --
   never, unasked, one the console draws on -- and with none it is not
   written at all. Only this image's: another program's
   StdErr (a tool's error line) still reaches the sessions, as on a screen
   in ErrOut. Undone before the port closes and before the unload, whose
   leak verdict is console output too -- back to the state it was found in,
   which the first call records. A false with no true before it (setup
   failed before the move, the image stayed resident, `sshd stop` later)
   changes nothing. */
static void
log_to_operator(bool on)
{
    bool now = axl_log_get_console_enabled();

    if (on && !m_log_moved) {
        m_log_before = now;
    }
    if (on || m_log_moved) {
        axl_log_set_console_serial(on ? m_serial : NULL);
    }
    axl_log_set_console_enabled(
        sshd_bind_log_console(on, m_log_moved, m_serial != NULL, m_log_before, now));
    m_log_moved = on;
}

/* net_up's progress hook: each step of the bring-up, named at DEBUG -- before
   it runs for the steps that may block (a pass, a driver, a DHCP) -- and what
   a failure's message needs counted. */
static void
net_progress(const AxlNetDriverEvent *ev, void *ctx)
{
    NetProgress *p  = (NetProgress *)ctx;
    bool         fw = axl_strcmp(ev->driver, "firmware") == 0;
    char         mac[18] = "";
    char         ip[16]  = "";
    const char  *what    = ev->is_embedded ? "embedded driver" : "driver";

    if (ev->have_nic) {
        axl_mac_format(ev->mac, mac, sizeof(mac));
    }
    if (ev->have_ip) {
        axl_ipv4_format(ev->ipv4, ip, sizeof(ip));
    }
    if (ev->is_dependency) {
        if (ev->outcome == AXL_NET_DRV_EV_UP) {
            axl_debug("loaded companion driver %s", ev->driver);
        } else {
            axl_debug("companion driver %s failed to load", ev->driver);
        }
        return;
    }
    switch (ev->outcome) {
    case AXL_NET_DRV_EV_TRYING:
        if (fw) {
            axl_debug("firmware pass: connecting the firmware's NIC drivers");
        } else {
            axl_debug("trying %s %s", what, ev->driver);
        }
        break;
    case AXL_NET_DRV_EV_LINK_DOWN: {
        /* A NIC is counted once however many passes report it (a swept
           driver's pass reports only its own NICs, but say so in code). */
        bool seen = false;
        for (size_t i = 0; i < p->down && !seen; i++) {
            seen = axl_memcmp(p->down_mac[i], ev->mac, 6) == 0;
        }
        if (!seen && p->down == p->down_cap) {
            size_t   cap  = p->down_cap > 0 ? p->down_cap * 2 : 8;
            uint8_t (*grown)[6] = axl_realloc(p->down_mac, cap * sizeof(*grown));
            if (grown == NULL) {
                p->down_lost = true;
                seen         = true;   /* not recorded: the message says "at least" */
            } else {
                p->down_mac = grown;
                p->down_cap = cap;
            }
        }
        if (!seen) {
            axl_memcpy(p->down_mac[p->down], ev->mac, 6);
            p->down++;
        }
        axl_debug("NIC %s link down -- skipping", mac);
        break;
    }
    case AXL_NET_DRV_EV_CONFIGURING: {
        /* What this DHCP may wait: its own attempt, cut to the deadline. */
        uint64_t now  = axl_time_get_ms();
        uint64_t left = now < p->until ? (p->until - now + 999u) / 1000u : 0;
        if (left > AXL_NET_DHCP_ATTEMPT_SEC) {
            left = AXL_NET_DHCP_ATTEMPT_SEC;
        }
        p->tried++;
        axl_debug("DHCP on NIC %s (up to %llu s)", mac, (unsigned long long)left);
        break;
    }
    case AXL_NET_DRV_EV_UP:
        if (fw) {
            axl_debug("firmware pass: NIC %s up with %s", mac, ip);
        } else {
            axl_debug("%s %s brought NIC %s up with %s", what, ev->driver, mac, ip);
        }
        break;
    case AXL_NET_DRV_EV_LINK_NO_LEASE:
        if (fw) {
            axl_debug("firmware pass: NIC %s got no address", mac);
        } else {
            axl_debug("%s %s: NIC %s got no address -- unloaded", what, ev->driver, mac);
        }
        break;
    case AXL_NET_DRV_EV_NO_NIC:
        if (fw) {
            axl_debug("firmware pass: no NIC with a link");
        } else {
            axl_debug("%s %s: no NIC with a link -- unloaded", what, ev->driver);
        }
        break;
    case AXL_NET_DRV_EV_LOAD_FAIL:
        axl_debug("%s %s failed to load", what, ev->driver);
        break;
    case AXL_NET_DRV_EV_SKIPPED_QUAR:
        axl_debug("skipping %s %s -- quarantined (it crashed a prior run; "
                  "'netload --clear' retries it)", what, ev->driver);
        break;
    case AXL_NET_DRV_EV_NO_REACH:   /* no verifier is installed: never sent */
        break;
    }
}

/* The NIC sshd may use already has an address: used as it is, as the
   bring-up always did -- nothing is connected, loaded or waited for. */
static bool
net_already_up(const SshdOpts *o)
{
    AxlNetInterface *ifs = NULL;
    size_t           n   = 0;
    bool             up  = false;
    char             mac[18];
    char             ip[16];

    if (axl_net_list_interfaces_alloc(&ifs, &n) != AXL_OK) {
        return false;
    }
    for (size_t i = 0; i < n && !up; i++) {
        if ((o->net.nic_index == AXL_NET_NIC_AUTO || i == o->net.nic_index)
            && ifs[i].has_ipv4) {
            axl_mac_format(ifs[i].mac, mac, sizeof(mac));
            axl_ipv4_format(ifs[i].ipv4, ip, sizeof(ip));
            axl_debug("NIC %s has %s -- using it", mac, ip);
            up = true;
        }
    }
    axl_free(ifs);
    return up;
}

#ifdef SSHD_EMBEDDED_NIC_DRIVER
/* The handle of the volume @p vol ("FS0:\..." -- the part before the colon)
   names, or NULL: the device the embedded NIC driver is given when there is
   no drivers directory, as if it had been loaded from the launcher's
   volume. */
static void *
volume_handle(const char *vol)
{
    const char *colon = vol != NULL ? axl_strchr(vol, ':') : NULL;
    AxlVolume   vols[32];
    size_t      n = 0;

    if (vol == NULL || vol[0] == '\0') {
        return NULL;   /* the launcher found none: nothing to say */
    }
    if (colon != NULL && colon != vol
        && axl_volume_enumerate(vols, AXL_ARRAY_SIZE(vols), &n) == AXL_OK) {
        for (size_t i = 0; i < n && i < AXL_ARRAY_SIZE(vols); i++) {
            if (axl_strlen(vols[i].name) == (size_t)(colon - vol)
                && axl_strncasecmp(vols[i].name, vol, (size_t)(colon - vol)) == 0) {
                return vols[i].handle;
            }
        }
    }
    axl_debug("the launcher's volume %s does not resolve -- the embedded NIC driver "
              "gets no device unless there is a drivers directory", vol);
    return NULL;
}
#endif

/* The network, before anything with side effects: the library's bring-up
   (axl_net_auto_init_opts, netload -a's engine) -- the firmware's NIC drivers
   first, then a crash-safe sweep of the drivers beside sshd.efi with their
   companions, skipping a NIC whose link is down -- under ONE deadline,
   AxlNetworkTimeout (60 s), that every wait in it is cut to and that stops the
   sweep, and ended by Ctrl-C: the break is the launcher's, which is waiting
   on this setup, so it is claimed only here. Each step is named at DEBUG
   before it runs, so a start that stalls says where; a failure is said on the
   console in sshd's own words, which the launcher's "could not start"
   follows. Loading and connecting a driver are firmware calls no deadline
   can cut short; the line before each names it. */
static int
net_up(const SshdOpts *o)
{
    bool        any = o->net.nic_index == AXL_NET_NIC_AUTO;
    const char *dir = (o->drivers != NULL && o->drivers[0] != '\0') ? o->drivers : NULL;
    char        nic[24];
    char        mac[18];
    /* The launcher bounds AxlNetworkTimeout; a value outside that (only an
       options string not of its making) is the default, not a refusal. */
    uint32_t    limit = o->net_timeout_sec;

    if (limit == 0 || limit > AXL_NET_AUTO_TIMEOUT_MAX_SEC) {
        limit = SSHD_NET_TIMEOUT_SEC;
    }
    if (any) {
        axl_strlcpy(nic, "auto", sizeof(nic));
    } else {
        axl_snprintf(nic, sizeof(nic), "%llu", (unsigned long long)o->net.nic_index);
    }
    axl_debug("bringing up the network (NIC %s, up to %u s)", nic, (unsigned)limit);
    if (net_already_up(o)) {
        return AXL_OK;
    }
    if (dir != NULL) {
        axl_debug("drivers directory: %s", dir);
    } else {
        axl_debug("no drivers directory found -- sweeping \\drivers\\" SSHD_DRIVERS_ARCH
                  " on every volume");
    }

    AxlCancellable *c = axl_cancellable_new();
    if (c == NULL) {
        return AXL_ERR;
    }
    axl_cancellable_set_cancel_on_break(c, true);
    NetProgress    prog = { .until = axl_time_get_ms() + (uint64_t)limit * 1000ULL };
    AxlNetAutoOpts ao   = {
        .nic_select       = any ? AXL_NET_NIC_SEL_AUTO : AXL_NET_NIC_SEL_INDEX,
        .nic_index        = any ? 0 : (size_t)o->net.nic_index,
        .dhcp_timeout_sec = AXL_NET_DHCP_ATTEMPT_SEC,   /* then on to the next NIC/driver */
        .driver_strategy  = AXL_NET_DRV_SWEEP_DIR,
        .sweep_dir        = dir,
        .deps             = axl_net_sweep_deps_sidecar(),
        .on_driver        = net_progress,
        .on_driver_ctx    = &prog,
        .timeout_sec      = limit,
        .cancel           = c,
        .link_wait_sec    = SSHD_LINK_WAIT_SEC,
    };
#ifdef SSHD_EMBEDDED_NIC_DRIVER
    /* The embedded name blob carries no NUL; it fits the quarantine key. */
    static char          emb_name[64];
    AxlNetEmbeddedDriver emb = { .name = emb_name };
    size_t               nl  = AXL_EMBED_SIZE(sshd_nic_name);

    if (nl >= sizeof(emb_name)) {
        nl = sizeof(emb_name) - 1;
    }
    axl_memcpy(emb_name, AXL_EMBED_DATA(sshd_nic_name), nl);
    emb_name[nl]      = '\0';
    emb.image         = AXL_EMBED_DATA(sshd_nic_driver);
    emb.size          = AXL_EMBED_SIZE(sshd_nic_driver);
    ao.embedded       = &emb;
    ao.embedded_count = 1;
    ao.device_handle  = volume_handle(o->volume);
#endif
    int rc = axl_net_auto_init_opts(&ao, NULL);
    axl_cancellable_set_cancel_on_break(c, false);
    axl_cancellable_free(c);

    size_t n = 0;
    switch (rc) {
    case AXL_OK:
        break;
    case AXL_CANCELLED:
        axl_printerr("sshd: network bring-up stopped (Ctrl-C)\n");
        break;
    case AXL_TIMEOUT:
        axl_printerr("sshd: network bring-up timed out after %u s\n", (unsigned)limit);
        break;
    default:
        if (axl_net_list_interfaces(NULL, &n) != AXL_OK) {
            n = 0;
        }
        if (prog.tried == 0 && prog.down > 0) {
            axl_mac_format(prog.down_mac[prog.down - 1], mac, sizeof(mac));
            if (!any) {
                axl_printerr("sshd: no network: NIC %llu (%s) has no link\n",
                             (unsigned long long)o->net.nic_index, mac);
            } else {
                axl_printerr("sshd: no network: no NIC has a link (%s%zu link down)\n",
                             prog.down_lost ? "at least " : "", prog.down);
            }
        } else if (prog.tried == 0 && n == 0) {
            axl_printerr("sshd: no network: no NIC driver produced an interface "
                         "(searched the firmware and %s%s)\n",
                         dir != NULL ? dir : "\\drivers\\" SSHD_DRIVERS_ARCH,
                         dir != NULL ? "" : " on every volume");
        } else if (prog.tried == 0 && !any) {
            axl_printerr("sshd: no NIC %llu (%zu found)\n",
                         (unsigned long long)o->net.nic_index, n);
        } else if (prog.tried > 0) {
            axl_printerr("sshd: no network: no DHCP lease on any NIC with a link "
                         "(%zu tried)\n", prog.tried);
        } else {
            axl_printerr("sshd: could not bring a NIC online\n");
        }
        break;
    }
    axl_free(prog.down_mac);
    return rc == AXL_OK ? AXL_OK : AXL_ERR;
}

/* Production's answer at every test point: never (see sshd-svc.h). */
__attribute__((weak)) bool
_sshd_test_point(const char *point)
{
    (void)point;
    return false;
}

/* Setup failed after the console was taken. The error return unloads the
   image, which is safe only once the firmware has its console back: a
   take-over that is still in place (stuck, or its device mid-call) calls into
   this image. In that case setup returns AXL_SERVICE_SETUP_HELD: the framework
   keeps the image resident and STOPPING (status says so), and `sshd stop`
   runs the teardown, which retries the release. */
static int
setup_failed(void)
{
    server_drop();
    int rc = console_drop();

    if (sshd_bind_setup_failed(sshd_bind_release_of(rc)) == SSHD_SETUP_FAIL) {
        return AXL_ERR;
    }
    axl_error("setup failed and the console take-over could not be undone (%d); "
              "staying resident - 'sshd stop' tries again", rc);
    m_state = SSHD_DONE;
    return AXL_SERVICE_SETUP_HELD;
}

static int
sshd_setup(AxlLoop *loop, void *user)
{
    SshdOpts *o = (SshdOpts *)user;
    uint32_t  cols = 0;
    uint32_t  rows = 0;

    /* First, so every line below is at the level asked for: LogLevel, -v or
       -d crossed in the options (this image's AxlLog is its own). */
    int level = sshd_bind_log_level(o->loglevel);
    if (level >= 0) {
        axl_log_set_level(level);
    }
    m_loop  = loop;
    m_state = SSHD_RUNNING;
    m_ro    = o->ro;
    m_password     = o->password != NULL ? o->password : "";
    m_password_len = axl_strlen(m_password);

    /* The launcher asked already; this catches another image winning the
       race, before anything with side effects. */
    if (axl_console_device_present()) {
        axl_error("another console take-over is active (fbcon or a BMC application)");
        return AXL_ERR;
    }
    if (!sshd_bind_can_log_in(o->pubkey, o->keys != NULL && o->keys[0] != '\0',
                              o->pwauth, m_password_len > 0)) {
        axl_error("no authorized_keys file and no password: nobody could log in");
        return AXL_ERR;
    }
    if (o->banner != NULL && o->banner[0] != '\0') {
        axl_debug("reading the banner %s", o->banner);
    }
    if (banner_read(o->banner) != AXL_OK) {
        return AXL_ERR;
    }
    if (log_port_pick(o->log_port) != AXL_OK) {
        return AXL_ERR;
    }
    if (net_up(o) != AXL_OK) {
        return AXL_ERR;
    }
    /* A resident service has no business with the Shell's Ctrl-C: it belongs
       to whatever the Shell is running. Driver-attached loops already leave
       it alone; said here so the intent does not rest on a default. */
    axl_loop_set_intercept_break(loop, false);

    /* The console first: its size is the banner, which must be in place
       before the first connection can arrive. */
    axl_debug("taking over the console");
    if (console_take() != AXL_OK) {
        axl_error("cannot take over the console");
        return AXL_ERR;
    }
    axl_debug("building the server (host key, authorized keys)");
    if (_sshd_test_point("setup") || server_build(o) != AXL_OK) {
        return setup_failed();
    }
    banner_sync();
    fit_sync();
    if (m_banner_cols == 0) {
        axl_error("cannot set the login banner that states the console size");
        return setup_failed();
    }
    axl_debug("listening on port %u", (unsigned)o->net.port);
    if (axl_ssh_server_listen(m_srv) != AXL_OK) {
        axl_error("cannot listen on port %u", (unsigned)o->net.port);
        return setup_failed();
    }
    m_tick = axl_loop_add_timer(loop, SSHD_TICK_MS, on_tick, NULL);
    if (m_tick == 0) {
        return setup_failed();
    }
    log_port_open(o->geometry);
    if (o->geometry && m_geom == NULL) {
        axl_error("--report-geometry: cannot open the first serial port");
        return setup_failed();
    }
    kept_keep(true);
    if (axl_protocol_install(&SSHD_HOSTKEY_GUID, &m_hk_iface, &m_hk_h) != AXL_OK) {
        m_hk_h = NULL;   /* `sshd status` then says it cannot see the key */
    }
    log_to_operator(true);
    m_shell_seen = axl_shell_kind() == AXL_SHELL_KIND_UEFI;
    m_muted      = false;
    axl_console_device_get_size(m_dev, &cols, &rows);
    char addrs[SSHD_ADDRS_MAX];
    sshd_addresses(addrs, sizeof(addrs));
    axl_info("serving the UEFI Shell on %s port %u (console %ux%u)%s",
             addrs, (unsigned)o->net.port, (unsigned)cols, (unsigned)rows,
             m_ro ? " read-only" : "");
    return AXL_OK;
}

/* `sshd stop` (or an unload) with sessions open: each gets the last word on
   the goodbye's path -- which resets a larger terminal's scroll region first,
   so the user's own shell gets the whole terminal back -- and then the loop
   is driven, briefly, for it to leave: the server's free drops connections
   abortively. Muted first: nothing the console prints from here is theirs.
   A session the Shell's exit already said goodbye to is not told twice. */
static void
stop_sessions(void)
{
    if (m_srv == NULL || m_state != SSHD_RUNNING || sshd_sessions_count(&m_sess) == 0) {
        return;
    }
    m_muted = true;
    sshd_sessions_each(&m_sess, SSHD_SESS_ANY, say_goodbye, (void *)SSHD_STOPPED_TEXT);
    uint64_t until = axl_time_get_ms() + SSHD_STOP_DRAIN_MS;
    while (sshd_sessions_count(&m_sess) > 0 && axl_time_get_ms() < until) {
        /* Anything but the slice elapsing (Ctrl-C, no room for the wake
           source) ends the wait: the drain is best effort. */
        if (axl_loop_iterate_until(m_loop, NULL, SSHD_STOP_SLICE_US) != AXL_TIMEOUT) {
            break;
        }
    }
}

/* The framework has already detached the driver tick, so nothing below can
   be preempted by the loop's tick. Server first (its hooks reach the device),
   then the device (it reaches the encoder), then the encoder. */
static int
sshd_teardown(void *user)
{
    (void)user;
    if (m_tick != 0) {
        axl_loop_remove_source(m_loop, m_tick);
        m_tick = 0;
    }
    stop_sessions();
    server_drop();
    int rc = console_drop();
    m_state = SSHD_DONE;
    if (rc != AXL_OK) {
        /* The device still calls into this image's code: refuse the unload
           rather than leave the console jumping into a freed image. */
        axl_error("unload refused: the console take-over is still in place (%d)", rc);
        return rc;
    }
    /* After the device is gone: until then a reshape could still report,
       and a log line could still reach a session through StdErr. */
    log_to_operator(false);
    kept_keep(false);
    if (m_hk_h != NULL) {
        axl_protocol_uninstall(m_hk_h, &SSHD_HOSTKEY_GUID, &m_hk_iface);
        m_hk_h = NULL;
    }
    m_geom = NULL;
    axl_serial_close(m_geom_own);
    m_geom_own = NULL;
    axl_serial_close(m_serial);
    m_serial = NULL;
    return AXL_OK;
}

AXL_SERVICE_DRIVER(sshd_service);

#endif /* AXL_SERVICE_BUILD_DRIVER */
